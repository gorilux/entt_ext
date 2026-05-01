#pragma once

// Authenticated-session token store for entt_ext::sync auth.
//
// Holds the mapping token → session_info with a TTL enforced on every
// validate() call. Time is passed in explicitly so tests can advance a
// fake clock instead of wall-clocking.
//
// Originally `nexus::auth::token_store`; promoted to
// `entt_ext::sync::token_store` as part of multi_tenant.md phase B.

#include <entt_ext/sync/user_account.hpp>

#include <openssl/rand.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace entt_ext::sync {

struct session_info {
  std::string                           username;
  user_role                             role = user_role::user;
  std::chrono::system_clock::time_point expires_at{};
};

class token_store {
public:
  struct config {
    std::chrono::seconds ttl         = std::chrono::hours(24);
    std::size_t          token_bytes = 32;
  };

  token_store()
    : config_()
    , sessions_() {}
  explicit token_store(config c)
    : config_(std::move(c)) {}

  std::string issue(std::string const& username, user_role role,
                    std::chrono::system_clock::time_point now) {
    auto token      = generate_token();
    auto expires_at = now + config_.ttl;
    sessions_.insert_or_assign(token, session_info{.username  = username,
                                                   .role      = role,
                                                   .expires_at = expires_at});
    return token;
  }

  session_info const* validate(std::string const&                    token,
                               std::chrono::system_clock::time_point now) {
    auto it = sessions_.find(token);
    if (it == sessions_.end()) return nullptr;
    if (now >= it->second.expires_at) {
      sessions_.erase(it);
      return nullptr;
    }
    return &it->second;
  }

  std::string refresh(std::string const&                    old_token,
                      std::chrono::system_clock::time_point now) {
    auto it = sessions_.find(old_token);
    if (it == sessions_.end() || now >= it->second.expires_at) {
      if (it != sessions_.end()) sessions_.erase(it);
      return {};
    }
    auto username = it->second.username;
    auto role     = it->second.role;
    sessions_.erase(it);
    return issue(username, role, now);
  }

  void revoke(std::string const& token) { sessions_.erase(token); }

  std::size_t sweep_expired(std::chrono::system_clock::time_point now) {
    std::size_t removed = 0;
    for (auto it = sessions_.begin(); it != sessions_.end();) {
      if (now >= it->second.expires_at) {
        it = sessions_.erase(it);
        ++removed;
      } else {
        ++it;
      }
    }
    return removed;
  }

  std::size_t   size() const noexcept { return sessions_.size(); }

  void          set_config(config c) { config_ = std::move(c); }
  config const& get_config() const noexcept { return config_; }

private:
  std::string generate_token() const {
    std::string bytes(config_.token_bytes, 0);
    auto*       data = reinterpret_cast<unsigned char*>(bytes.data());
    if (RAND_bytes(data, static_cast<int>(bytes.size())) != 1) {
      throw std::runtime_error("RAND_bytes failed");
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string           out;
    out.reserve(bytes.size() * 2);
    for (unsigned char c : bytes) {
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0x0f]);
    }
    return out;
  }

  config                                        config_;
  std::unordered_map<std::string, session_info> sessions_;
};

} // namespace entt_ext::sync
