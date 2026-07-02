#pragma once

// Password hashing helpers for entt_ext::sync.
//
// Stored hashes are in the format:
//     pbkdf2$<iterations>$<salt_hex>$<hash_hex>
// Everything needed to verify is in the string — no extra salt column. The
// legacy SHA-256-with-external-salt format is accepted by verify() for
// migration; needs_rehash() tells callers when to upgrade on next login.
//
// Originally shipped under `nexus::auth::password`; promoted to
// `entt_ext::sync::password` as part of multi_tenant.md phase B so other
// apps (gym, cnc) can reuse the same implementation. Nexus keeps a thin
// alias-based shim for backward compatibility.

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <fcntl.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace entt_ext::sync::password {

namespace detail {

inline std::string to_hex(unsigned char const* data, std::size_t len) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string           out;
  out.reserve(len * 2);
  for (std::size_t i = 0; i < len; ++i) {
    out.push_back(kHex[data[i] >> 4]);
    out.push_back(kHex[data[i] & 0x0f]);
  }
  return out;
}

inline std::vector<unsigned char> from_hex(std::string_view hex) {
  if (hex.size() % 2 != 0) return {};
  std::vector<unsigned char> out;
  out.reserve(hex.size() / 2);
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (std::size_t i = 0; i < hex.size(); i += 2) {
    int hi = nibble(hex[i]);
    int lo = nibble(hex[i + 1]);
    if (hi < 0 || lo < 0) return {};
    out.push_back(static_cast<unsigned char>((hi << 4) | lo));
  }
  return out;
}

inline bool constant_time_eq(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

inline bool starts_with(std::string_view s, std::string_view prefix) {
  return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

// Split on a single character. Skips empty trailing chunks.
inline std::vector<std::string_view> split(std::string_view s, char sep) {
  std::vector<std::string_view> out;
  std::size_t                   i = 0;
  while (i <= s.size()) {
    auto j = s.find(sep, i);
    if (j == std::string_view::npos) j = s.size();
    out.emplace_back(s.substr(i, j - i));
    i = j + 1;
  }
  return out;
}

} // namespace detail

// NIST SP 800-63B recommends ≥ 600k PBKDF2-HMAC-SHA256 iterations as of
// 2023. Take ~100 ms on a modern CPU; fast enough that login feels instant,
// slow enough that a GPU brute-force is prohibitive.
inline constexpr int kDefaultIterations = 600000;
inline constexpr int kSaltBytes         = 16;
inline constexpr int kHashBytes         = 32;

inline std::string generate_salt(std::size_t bytes = kSaltBytes) {
  std::vector<unsigned char> buf(bytes);
  if (RAND_bytes(buf.data(), static_cast<int>(bytes)) != 1) {
    throw std::runtime_error("RAND_bytes failed");
  }
  return detail::to_hex(buf.data(), bytes);
}

// Compute the legacy hash that the old auth_module.cpp used to produce.
// Exposed so both verify() and migration callers speak the same format.
inline std::string legacy_sha256_hash(std::string const& plaintext,
                                      std::string const& salt) {
  std::string   input = salt + plaintext;
  unsigned char out[SHA256_DIGEST_LENGTH];
  unsigned int  len = 0;
  EVP_MD_CTX*   ctx = EVP_MD_CTX_new();
  EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
  EVP_DigestUpdate(ctx, input.data(), input.size());
  EVP_DigestFinal_ex(ctx, out, &len);
  EVP_MD_CTX_free(ctx);
  return detail::to_hex(out, len);
}

// Hash a plaintext password. Returns the full self-describing string that
// should be stored verbatim in user_account.password_hash.
inline std::string hash(std::string const& plaintext,
                        int                iterations = kDefaultIterations) {
  auto salt       = generate_salt();
  auto salt_bytes = detail::from_hex(salt);

  std::vector<unsigned char> out(kHashBytes);
  if (PKCS5_PBKDF2_HMAC(plaintext.data(), static_cast<int>(plaintext.size()),
                        salt_bytes.data(), static_cast<int>(salt_bytes.size()),
                        iterations, EVP_sha256(),
                        static_cast<int>(out.size()), out.data()) != 1) {
    throw std::runtime_error("PKCS5_PBKDF2_HMAC failed");
  }
  return std::format("pbkdf2${}${}${}", iterations, salt,
                     detail::to_hex(out.data(), out.size()));
}

// Verify plaintext against a stored hash. Returns true iff the password
// matches. Supports both the new pbkdf2$... format and the legacy
// SHA-256-hex format; the legacy path needs the caller to pass the salt
// from user_account.salt.
inline bool verify(std::string const& stored,
                   std::string const& plaintext,
                   std::string const& legacy_salt = "") {
  if (stored.empty()) return false;

  if (detail::starts_with(stored, "pbkdf2$")) {
    auto parts = detail::split(stored, '$');
    // Expect 4 parts: ["pbkdf2", iterations, salt_hex, hash_hex]
    if (parts.size() != 4) return false;
    int iterations = 0;
    try {
      iterations = std::stoi(std::string(parts[1]));
    } catch (...) {
      return false;
    }
    if (iterations <= 0) return false;

    auto salt_bytes = detail::from_hex(parts[2]);
    auto expected   = detail::from_hex(parts[3]);
    if (salt_bytes.empty() || expected.empty()) return false;

    std::vector<unsigned char> got(expected.size());
    if (PKCS5_PBKDF2_HMAC(plaintext.data(), static_cast<int>(plaintext.size()),
                          salt_bytes.data(), static_cast<int>(salt_bytes.size()),
                          iterations, EVP_sha256(),
                          static_cast<int>(got.size()), got.data()) != 1) {
      return false;
    }
    return detail::constant_time_eq(
        std::string_view{reinterpret_cast<char const*>(got.data()), got.size()},
        std::string_view{reinterpret_cast<char const*>(expected.data()), expected.size()});
  }

  // Legacy branch: stored is a bare hex SHA-256 digest. Needs the original
  // salt to reproduce the hash for comparison.
  if (legacy_salt.empty()) return false;
  return detail::constant_time_eq(stored, legacy_sha256_hash(plaintext, legacy_salt));
}

// True when the stored hash is either empty or in the legacy format and
// should be upgraded on the next successful login.
inline bool needs_rehash(std::string const& stored) {
  return !detail::starts_with(stored, "pbkdf2$");
}

// Generate a random, human-typeable one-time password from the CSPRNG. Uses a
// 32-symbol alphabet with the ambiguous glyphs removed (no 0/O, 1/I) so it can
// be read off a console and typed once. 256 is divisible by 32, so `byte % 32`
// is unbiased. `length` symbols ⇒ length*5 bits of entropy (default 24 ⇒ 120).
inline std::string generate_random_password(std::size_t length = 24) {
  static constexpr char kAlphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"; // 32 chars, no 0/O/1/I
  std::vector<unsigned char> buf(length);
  if (RAND_bytes(buf.data(), static_cast<int>(length)) != 1) {
    throw std::runtime_error("RAND_bytes failed");
  }
  std::string out;
  out.reserve(length);
  for (unsigned char c : buf) out.push_back(kAlphabet[c % 32]);
  return out;
}

// Write a secret (e.g. a generated bootstrap password) to `path`, creating it
// with mode 0600 ATOMICALLY (permissions applied at open, so there is no
// world-readable window as a create-then-chmod would have). Returns false on
// any I/O failure. Server-side only; harmless if this header is included on a
// client that never calls it.
inline bool write_secret_file(std::string const& path, std::string const& contents) {
  int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) return false;
  std::string const data = contents + "\n";
  std::size_t        off = 0;
  bool               ok  = true;
  while (off < data.size()) {
    ssize_t n = ::write(fd, data.data() + off, data.size() - off);
    if (n <= 0) { ok = false; break; }
    off += static_cast<std::size_t>(n);
  }
  ::close(fd);
  return ok;
}

} // namespace entt_ext::sync::password
