#pragma once

// Failed-login throttle: per-username lockout AND per-source-IP throttle.
//
//   * per-username (the original): defends a single account from targeted
//     password-spraying. A window of consecutive failures trips a fixed lockout.
//   * per-source-IP (WI-15): defends against spraying MANY usernames from one
//     source — which the per-username counter never sees — and is the primary
//     brake, since locking an account by guessing its name is itself a DoS.
//
// Both maps are size-bounded (WI-15) so a flood of unique usernames/IPs cannot
// grow memory without bound. Time is injected on every call so tests can drive
// it deterministically.
//
// Originally `nexus::auth::login_rate_limiter`. Promoted to
// `entt_ext::sync::login_rate_limiter` as part of multi_tenant.md phase B.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <utility>

namespace entt_ext::sync {

class login_rate_limiter {
public:
  struct config {
    // Per-username lockout.
    std::size_t          max_failures = 5;
    std::chrono::seconds window       = std::chrono::seconds(60);
    std::chrono::seconds lockout      = std::chrono::minutes(5);

    // Per-source-IP throttle (WI-15). More lenient than the per-account lock —
    // one IP may host several legitimate users — but still bounds a sprayer.
    std::size_t          ip_max_failures = 20;
    std::chrono::seconds ip_window       = std::chrono::seconds(60);
    std::chrono::seconds ip_lockout      = std::chrono::minutes(5);

    // Hard cap on tracked keys PER map (WI-15). Oldest entries are evicted so a
    // flood of unique usernames or IPs cannot grow memory unbounded.
    std::size_t          max_tracked = 8192;
  };

  login_rate_limiter() = default;
  explicit login_rate_limiter(config c)
    : config_(std::move(c)) {}

  // ---- per-username ----
  bool is_locked(std::string const& username, std::chrono::system_clock::time_point now) const {
    return locked_(tracker_, username, now);
  }
  bool record_failure(std::string const& username, std::chrono::system_clock::time_point now) {
    return record_(tracker_, username, config_.max_failures, config_.window, config_.lockout, now);
  }
  void record_success(std::string const& username) { tracker_.erase(username); }
  std::size_t consecutive_failures(std::string const& username) const {
    auto it = tracker_.find(username);
    return it == tracker_.end() ? 0 : it->second.consecutive_failures;
  }

  // ---- per-source-IP (WI-15). No-op when ip is empty (e.g. unit-test direct
  // dispatch with no transport), so callers can pass it unconditionally. ----
  bool is_ip_throttled(std::string const& ip, std::chrono::system_clock::time_point now) const {
    return ip.empty() ? false : locked_(ip_tracker_, ip, now);
  }
  bool record_ip_failure(std::string const& ip, std::chrono::system_clock::time_point now) {
    if (ip.empty()) return false;
    return record_(ip_tracker_, ip, config_.ip_max_failures, config_.ip_window, config_.ip_lockout, now);
  }
  void record_ip_success(std::string const& ip) {
    if (!ip.empty()) ip_tracker_.erase(ip);
  }

  void          clear() { tracker_.clear(); ip_tracker_.clear(); }
  void          set_config(config c) { config_ = std::move(c); }
  config const& get_config() const noexcept { return config_; }

private:
  using time_point = std::chrono::system_clock::time_point;
  struct entry {
    std::size_t consecutive_failures = 0;
    time_point  last_failure_at{};
    time_point  locked_until{};
  };
  using map_t = std::unordered_map<std::string, entry>;

  static bool locked_(map_t const& m, std::string const& key, time_point now) {
    auto it = m.find(key);
    if (it == m.end()) return false;
    return now < it->second.locked_until;
  }

  bool record_(map_t& m, std::string const& key, std::size_t max_failures,
               std::chrono::seconds window, std::chrono::seconds lockout, time_point now) {
    auto it = m.find(key);
    if (it == m.end()) {
      evict_if_full_(m, now);
      it = m.emplace(key, entry{}).first;
    }
    auto& e = it->second;
    if (e.last_failure_at != time_point{} && now - e.last_failure_at > window) {
      e.consecutive_failures = 0;
    }
    ++e.consecutive_failures;
    e.last_failure_at = now;
    bool newly_locked = false;
    if (e.consecutive_failures >= max_failures) {
      auto was_locked = e.locked_until > now;
      e.locked_until  = now + lockout;
      newly_locked    = !was_locked;
    }
    return newly_locked;
  }

  // Keep the map bounded (WI-15). Called only before inserting a NEW key. First
  // drop entries whose lockout has expired and whose last failure is stale; if
  // still at capacity, evict the least-recently-failed entry.
  void evict_if_full_(map_t& m, time_point now) {
    if (m.size() < config_.max_tracked) return;
    auto const stale = std::max(config_.window, config_.ip_window);
    for (auto it = m.begin(); it != m.end();) {
      if (now >= it->second.locked_until && now - it->second.last_failure_at > stale) {
        it = m.erase(it);
      } else {
        ++it;
      }
    }
    if (m.size() < config_.max_tracked) return;
    auto oldest = m.begin();
    for (auto it = m.begin(); it != m.end(); ++it) {
      if (it->second.last_failure_at < oldest->second.last_failure_at) oldest = it;
    }
    if (oldest != m.end()) m.erase(oldest);
  }

  config config_;
  map_t  tracker_;     // per-username
  map_t  ip_tracker_;  // per-source-IP (WI-15)
};

} // namespace entt_ext::sync
