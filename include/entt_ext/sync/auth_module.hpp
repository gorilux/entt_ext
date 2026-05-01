#pragma once

// Multi-tenant auth module for entt_ext::sync.
//
// Holds the per-server auth state (rate limiter + token store) and
// exposes:
//   - auth_handler(): a callback the app passes to
//     sync_server::set_auth_handler. Validates username/password
//     against persisted `user_account` components, applies rate
//     limiting, issues a token on success.
//   - validate_token / refresh_token / revoke_token: post-handshake
//     entry points the app's RPC layer can wire to.
//
// User-management RPC endpoints (create_user, change_password, list,
// auth_refresh) are exposed as a separate
// `register_user_management_endpoints<Server>(server, module, ecs)`
// free function below — that function is templated on the rpc::server
// type because each app builds the server with its own channel.
//
// Originally `nexus::auth::module`. Promoted to
// `entt_ext::sync::auth_module` as part of multi_tenant.md phase B.

#include <entt_ext/ecs.hpp>
#include <entt_ext/sync/login_rate_limiter.hpp>
#include <entt_ext/sync/password.hpp>
#include <entt_ext/sync/token_store.hpp>
#include <entt_ext/sync/user_account.hpp>
#include <entt_ext/sync_common.hpp>

#include <spdlog/spdlog.h>

#include <chrono>
#include <functional>
#include <optional>
#include <string>

namespace entt_ext::sync {

class auth_module {
public:
  struct config {
    login_rate_limiter::config rate_limit{};
    token_store::config        tokens{};
    // Optional bootstrap admin. Created with the given password if no
    // user_account components exist after deserialization. Username
    // empty disables bootstrap (apps that prefer to seed users out of
    // band).
    std::string bootstrap_admin_username = "admin";
    std::string bootstrap_admin_password = "admin";
  };

  explicit auth_module(ecs& e)
    : auth_module(e, config{}) {}

  auth_module(ecs& e, config cfg)
    : ecs_(e)
    , cfg_(std::move(cfg))
    , rate_limiter_(cfg_.rate_limit)
    , tokens_(cfg_.tokens) {}

  // The callback to hand to sync_server::set_auth_handler. Captures
  // `this`; auth_module must outlive the sync_server.
  std::function<handshake_response(handshake_request const&)> auth_handler() {
    return [this](handshake_request const& request) -> handshake_response {
      auto const now = std::chrono::system_clock::now();

      if (request.username.empty()) {
        return {.success       = false,
                .error_message = "Authentication required: please provide username and password"};
      }

      if (rate_limiter_.is_locked(request.username, now)) {
        spdlog::warn("auth: rejecting locked user '{}'", request.username);
        return {.success = false, .error_message = "Account temporarily locked — try again later"};
      }

      for (auto [e, acc] : ecs_.view<user_account>().each()) {
        if (acc.username != request.username) continue;

        if (!password::verify(acc.password_hash, request.password, acc.salt)) {
          bool newly_locked = rate_limiter_.record_failure(request.username, now);
          if (newly_locked) {
            spdlog::warn("auth: user '{}' locked out after repeated failures", request.username);
          } else {
            spdlog::info("auth: bad password for '{}' (failures={})",
                         request.username,
                         rate_limiter_.consecutive_failures(request.username));
          }
          return {.success = false, .error_message = "Invalid password"};
        }

        rate_limiter_.record_success(request.username);

        // Migration: a successful verify against a legacy SHA-256 hash
        // is the one chance to upgrade. Rehash with PBKDF2, drop the
        // external salt.
        if (password::needs_rehash(acc.password_hash)) {
          acc.password_hash = password::hash(request.password);
          acc.salt          = "";
          ecs_.replace<user_account>(e, acc);
          spdlog::info("auth: upgraded legacy hash for user '{}'", acc.username);
        }

        auto token = tokens_.issue(acc.username, acc.role, now);
        spdlog::info("auth: user '{}' authenticated (role: {})",
                     acc.username, static_cast<int>(acc.role));
        return {.success    = true,
                .role       = static_cast<int>(acc.role),
                .auth_token = token};
      }

      // Unknown username still counts as a failure for rate-limiting so
      // an attacker can't probe the user list for free.
      rate_limiter_.record_failure(request.username, now);
      return {.success       = false,
              .error_message = "User '" + request.username + "' not found"};
    };
  }

  // Look up the session for a token. Returns nullptr on unknown /
  // expired. validate() lazily evicts expired entries.
  session_info const* validate_token(std::string const& token) {
    return tokens_.validate(token, std::chrono::system_clock::now());
  }

  std::string refresh_token(std::string const& old_token) {
    return tokens_.refresh(old_token, std::chrono::system_clock::now());
  }

  void revoke_token(std::string const& token) { tokens_.revoke(token); }

  bool has_user(std::string const& username) const {
    for (auto [e, acc] : const_cast<ecs&>(ecs_).view<user_account>().each()) {
      if (acc.username == username) return true;
    }
    return false;
  }

  std::optional<user_account> find_user(std::string const& username) const {
    for (auto [e, acc] : const_cast<ecs&>(ecs_).view<user_account>().each()) {
      if (acc.username == username) return acc;
    }
    return std::nullopt;
  }

  // Apps that want a bootstrap admin call this once after the
  // persistence module has finished deserializing. No-op when any
  // user_account already exists.
  void create_bootstrap_admin_if_needed() {
    if (cfg_.bootstrap_admin_username.empty()) return;

    if (ecs_.view<user_account>().size() > 0) {
      spdlog::info("auth: {} user account(s) found", ecs_.view<user_account>().size());
      return;
    }

    auto hash = password::hash(cfg_.bootstrap_admin_password);
    auto e    = ecs_.create();
    ecs_.emplace<user_account>(e,
                               user_account{.username      = cfg_.bootstrap_admin_username,
                                            .password_hash = hash,
                                            .salt          = "",
                                            .role          = user_role::admin});
    spdlog::info("auth: created bootstrap admin '{}'", cfg_.bootstrap_admin_username);
  }

  // De-duplicate user_account entities. Old init-order bugs could
  // produce duplicates when the auth module ran before persistence
  // loaded; the new lifecycle prevents that, but the cleanup helper is
  // useful when migrating from such a deployment.
  std::size_t remove_duplicate_users() {
    std::unordered_map<std::string, entity> seen;
    std::vector<entity>                     duplicates;
    for (auto [e, acc] : ecs_.view<user_account>().each()) {
      auto [it, inserted] = seen.try_emplace(acc.username, e);
      if (!inserted) duplicates.push_back(e);
    }
    for (auto e : duplicates) ecs_.destroy(e);
    if (!duplicates.empty()) {
      spdlog::warn("auth: removed {} duplicate user account(s)", duplicates.size());
    }
    return duplicates.size();
  }

  config const& get_config() const noexcept { return cfg_; }

private:
  ecs&               ecs_;
  config             cfg_;
  login_rate_limiter rate_limiter_;
  token_store        tokens_;
};

// ============================================================================
// User-management RPC endpoints. Templated on the rpc::server type so
// each app can wire it up against its own channel without entt_ext
// pulling in a hard dependency on a specific channel pick. Apps call
// this once after constructing both the server and the auth_module.
// ============================================================================
template <typename Server>
void register_user_management_endpoints(Server& server, auth_module& auth, ecs& ecs_ref) {
  // Only authenticated callers; the auth check is enforced by the
  // server's auth_callback, which the app should configure to match
  // its dispatch::visibility expectations. The endpoints below all
  // re-validate the token explicitly so we still defend in depth.

  server.attach("auth.create_user",
                [&auth, &ecs_ref](create_user_request const& req) -> user_mgmt_response {
                  auto* sess = auth.validate_token(req.auth_token);
                  if (sess == nullptr) {
                    return {.success = false, .error_message = "Invalid or expired token"};
                  }
                  if (sess->role != user_role::admin) {
                    return {.success = false, .error_message = "Admin role required"};
                  }
                  if (req.username.empty() || req.password.empty()) {
                    return {.success = false, .error_message = "Username and password required"};
                  }
                  if (auth.has_user(req.username)) {
                    return {.success = false, .error_message = "User already exists"};
                  }
                  auto e = ecs_ref.create();
                  ecs_ref.template emplace<user_account>(e,
                                                         user_account{.username      = req.username,
                                                                      .password_hash = password::hash(req.password),
                                                                      .salt          = "",
                                                                      .role          = req.role});
                  return {.success = true};
                });

  server.attach("auth.delete_user",
                [&auth, &ecs_ref](delete_user_request const& req) -> user_mgmt_response {
                  auto* sess = auth.validate_token(req.auth_token);
                  if (sess == nullptr) {
                    return {.success = false, .error_message = "Invalid or expired token"};
                  }
                  if (sess->role != user_role::admin) {
                    return {.success = false, .error_message = "Admin role required"};
                  }
                  if (sess->username == req.username) {
                    return {.success = false, .error_message = "Cannot delete your own account"};
                  }
                  for (auto [e, acc] : ecs_ref.template view<user_account>().each()) {
                    if (acc.username == req.username) {
                      ecs_ref.destroy(e);
                      return {.success = true};
                    }
                  }
                  return {.success = false, .error_message = "User not found"};
                });

  server.attach("auth.change_password",
                [&auth, &ecs_ref](change_password_request const& req) -> user_mgmt_response {
                  auto* sess = auth.validate_token(req.auth_token);
                  if (sess == nullptr) {
                    return {.success = false, .error_message = "Invalid or expired token"};
                  }
                  bool changing_own = (sess->username == req.username);
                  if (!changing_own && sess->role != user_role::admin) {
                    return {.success = false, .error_message = "Admin role required to change another user's password"};
                  }
                  if (req.new_password.empty()) {
                    return {.success = false, .error_message = "New password required"};
                  }
                  for (auto [e, acc] : ecs_ref.template view<user_account>().each()) {
                    if (acc.username != req.username) continue;

                    if (changing_own &&
                        !password::verify(acc.password_hash, req.old_password, acc.salt)) {
                      return {.success = false, .error_message = "Old password is incorrect"};
                    }
                    acc.password_hash = password::hash(req.new_password);
                    acc.salt          = "";
                    ecs_ref.template replace<user_account>(e, acc);
                    return {.success = true};
                  }
                  return {.success = false, .error_message = "User not found"};
                });

  server.attach("auth.list_users",
                [&auth, &ecs_ref](list_users_request const& req) -> list_users_response {
                  auto* sess = auth.validate_token(req.auth_token);
                  if (sess == nullptr) {
                    return {.success = false, .error_message = "Invalid or expired token"};
                  }
                  if (sess->role != user_role::admin) {
                    return {.success = false, .error_message = "Admin role required"};
                  }
                  list_users_response resp{.success = true};
                  for (auto [e, acc] : ecs_ref.template view<user_account>().each()) {
                    resp.users.push_back(user_info{.username = acc.username, .role = acc.role});
                  }
                  return resp;
                });

  server.attach("auth.refresh",
                [&auth](auth_refresh_request const& req) -> auth_refresh_response {
                  auto* sess = auth.validate_token(req.auth_token);
                  if (sess == nullptr) {
                    return {.success = false, .error_message = "Invalid or expired token"};
                  }
                  auto new_tok = auth.refresh_token(req.auth_token);
                  if (new_tok.empty()) {
                    return {.success = false, .error_message = "Refresh failed"};
                  }
                  return {.success = true, .new_auth_token = new_tok};
                });
}

} // namespace entt_ext::sync
