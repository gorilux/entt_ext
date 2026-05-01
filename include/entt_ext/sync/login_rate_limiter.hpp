#pragma once

// Per-user failed-login counter + temporary lockout.
//
// Defends against password-spraying a single username. Decouples the two
// orthogonal concerns that usually get mixed up in ad-hoc code:
//   * the window: consecutive failures only accumulate while they stay
//     close together — a trickle of wrong guesses across an afternoon
//     should not end in a lockout
//   * the lockout: once the threshold trips, the account is locked for a
//     fixed duration regardless of further attempts
//
// Time is injected on every call so tests can drive it deterministically.
//
// Originally `nexus::auth::login_rate_limiter`. Promoted to
// `entt_ext::sync::login_rate_limiter` as part of multi_tenant.md phase B.

#include <chrono>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <utility>

namespace entt_ext::sync {

class login_rate_limiter {
public:
  struct config {
    std::size_t          max_failures = 5;
    std::chrono::seconds window       = std::chrono::seconds(60);
    std::chrono::seconds lockout      = std::chrono::minutes(5);
  };

  login_rate_limiter()
    : config_()
    , tracker_() {}
  explicit login_rate_limiter(config c)
    : config_(std::move(c)) {}

  bool is_locked(std::string const&                    username,
                 std::chrono::system_clock::time_point now) const {
    auto it = tracker_.find(username);
    if (it == tracker_.end()) return false;
    return now < it->second.locked_until;
  }

  bool record_failure(std::string const&                    username,
                      std::chrono::system_clock::time_point now) {
    auto& entry = tracker_[username];
    if (entry.last_failure_at != std::chrono::system_clock::time_point{} &&
        now - entry.last_failure_at > config_.window) {
      entry.consecutive_failures = 0;
    }
    ++entry.consecutive_failures;
    entry.last_failure_at = now;
    bool newly_locked     = false;
    if (entry.consecutive_failures >= config_.max_failures) {
      auto was_locked    = entry.locked_until > now;
      entry.locked_until = now + config_.lockout;
      newly_locked       = !was_locked;
    }
    return newly_locked;
  }

  void record_success(std::string const& username) { tracker_.erase(username); }

  std::size_t consecutive_failures(std::string const& username) const {
    auto it = tracker_.find(username);
    return it == tracker_.end() ? 0 : it->second.consecutive_failures;
  }
  void clear() { tracker_.clear(); }

  void          set_config(config c) { config_ = std::move(c); }
  config const& get_config() const noexcept { return config_; }

private:
  struct entry {
    std::size_t                           consecutive_failures = 0;
    std::chrono::system_clock::time_point last_failure_at{};
    std::chrono::system_clock::time_point locked_until{};
  };

  config                                 config_;
  std::unordered_map<std::string, entry> tracker_;
};

} // namespace entt_ext::sync
