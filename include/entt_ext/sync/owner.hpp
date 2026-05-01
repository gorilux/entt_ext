#pragma once

// Multi-tenant ownership marker (see docs/multi_tenant.md).
//
// Stamped on every entity by sync_server when the owning session first
// creates it. Treated as server-only — the snapshot and notification
// paths filter on it; clients never see it directly.
//
// `user_id` is the username string returned from the handshake's
// auth_handler. An empty user_id ("") means "unowned / global" and is
// visible to every session — preserves single-tenant behavior in
// deployments without an auth_handler.

#include <cereal/cereal.hpp>

#include <cstdint>
#include <string>

namespace entt_ext::sync {

struct owner {
  std::string user_id;

  template <typename Archive>
  void serialize(Archive& ar, std::uint32_t const /*version*/) {
    ar(user_id);
  }
};

} // namespace entt_ext::sync

CEREAL_CLASS_VERSION(entt_ext::sync::owner, 1);
