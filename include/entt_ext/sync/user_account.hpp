#pragma once

// User identity primitives for entt_ext::sync.
//
// `user_account` is a server-side persisted component representing a
// password-authenticated user. `user_role` distinguishes regular users
// from admins (admins bypass the multi-tenant ownership filter — see
// docs/multi_tenant.md).
//
// `auth_session` is the matching client-side persistable record so the
// app can keep a user logged in across process restarts.
//
// Originally `nexus::user_account` etc; promoted under
// `entt_ext::sync::` as part of multi_tenant.md phase B.

#include <cereal/cereal.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/vector.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace entt_ext::sync {

enum class user_role : int {
  user  = 0,
  admin = 1
};

// Server-persisted user account. Never synced to clients (it carries the
// password hash). Lives in the server's settings file alongside the rest
// of the persistent state.
struct user_account {
  std::string username;
  std::string password_hash;       // pbkdf2$… or legacy hex sha256
  std::string salt;                // legacy salt; empty after migration
  user_role   role = user_role::user;

  template <typename Archive>
  void save(Archive& ar, std::uint32_t const /*version*/) const {
    int r = static_cast<int>(role);
    ar(username, password_hash, salt, r);
  }

  template <typename Archive>
  void load(Archive& ar, std::uint32_t const /*version*/) {
    int r = 0;
    ar(username, password_hash, salt, r);
    role = static_cast<user_role>(r);
  }
};

// Client-local auth session. Apps that want "remember me" persist this
// alongside their connection config and pass username + token back on
// the next handshake.
struct auth_session {
  std::string username;
  user_role   role = user_role::user;
  std::string auth_token;

  template <typename Archive>
  void save(Archive& ar, std::uint32_t const /*version*/) const {
    int r = static_cast<int>(role);
    ar(username, r, auth_token);
  }

  template <typename Archive>
  void load(Archive& ar, std::uint32_t const /*version*/) {
    int r = 0;
    ar(username, r, auth_token);
    role = static_cast<user_role>(r);
  }
};

// ============================================================================
// User-management RPC types. Apps that expose admin endpoints reuse these
// directly; apps that don't can ignore them.
// ============================================================================

struct create_user_request {
  std::string auth_token;
  std::string username;
  std::string password;
  user_role   role = user_role::user;

  template <typename Archive>
  void save(Archive& ar) const {
    int r = static_cast<int>(role);
    ar(auth_token, username, password, r);
  }

  template <typename Archive>
  void load(Archive& ar) {
    int r = 0;
    ar(auth_token, username, password, r);
    role = static_cast<user_role>(r);
  }
};

struct delete_user_request {
  std::string auth_token;
  std::string username;

  template <typename Archive>
  void serialize(Archive& ar) {
    ar(auth_token, username);
  }
};

struct change_password_request {
  std::string auth_token;
  std::string username;
  std::string old_password;
  std::string new_password;

  template <typename Archive>
  void serialize(Archive& ar) {
    ar(auth_token, username, old_password, new_password);
  }
};

struct user_mgmt_response {
  bool        success = false;
  std::string error_message;

  template <typename Archive>
  void serialize(Archive& ar) {
    ar(success, error_message);
  }
};

struct user_info {
  std::string username;
  user_role   role = user_role::user;

  template <typename Archive>
  void save(Archive& ar) const {
    int r = static_cast<int>(role);
    ar(username, r);
  }

  template <typename Archive>
  void load(Archive& ar) {
    int r = 0;
    ar(username, r);
    role = static_cast<user_role>(r);
  }
};

struct list_users_request {
  std::string auth_token;

  template <typename Archive>
  void serialize(Archive& ar) {
    ar(auth_token);
  }
};

struct list_users_response {
  bool                   success = false;
  std::string            error_message;
  std::vector<user_info> users;

  template <typename Archive>
  void serialize(Archive& ar) {
    ar(success, error_message, users);
  }
};

struct auth_refresh_request {
  std::string auth_token;

  template <typename Archive>
  void serialize(Archive& ar) {
    ar(auth_token);
  }
};

struct auth_refresh_response {
  bool        success = false;
  std::string error_message;
  std::string new_auth_token;

  template <typename Archive>
  void serialize(Archive& ar) {
    ar(success, error_message, new_auth_token);
  }
};

} // namespace entt_ext::sync

CEREAL_CLASS_VERSION(entt_ext::sync::user_account, 0)
CEREAL_CLASS_VERSION(entt_ext::sync::auth_session, 0)
