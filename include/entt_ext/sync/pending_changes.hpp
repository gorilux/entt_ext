#pragma once

// Phase 3 of the offline-first plan (see docs/offline_first.md).
//
// Empty/lightweight marker components stamped on entities by the
// sync_client's component observers when a mutation cannot be sent to
// the server (offline, send failed, or the component was created
// before the first connect). The cache module persists them alongside
// regular components, so they survive a process restart. Phase 4's
// reconcile path drains them on reconnect.
//
// Per-component, per-entity. The semantics:
//   - pending_create<T> on entity e:
//       "client created (or restored from cache) component T on e
//        without server-side acknowledgement"
//       Clearing rule: server confirmed creation in reconcile.
//   - pending_update<T> on entity e:
//       "client modified T on e without server-side acknowledgement"
//       Carries the wall-clock time of the most recent edit, used for
//       last-write-wins ordering on the server.
//       Clearing rule: server confirmed apply in reconcile.
//
// pending_delete is intentionally omitted at this phase — entity-level
// deletes need a tombstone on the *global* entity (the dying entity
// can't carry markers). It's a phase 4 concern.

#include <cstdint>

namespace entt_ext::sync {

template <typename T>
struct pending_create {
  // Empty marker. The serialize is required because the client_state_cache
  // round-trips this storage through cereal so the marker survives a
  // process restart; without it the empty struct fails the
  // serialization-function lookup at template instantiation time.
  template <typename Archive>
  void serialize(Archive&) {}
};

template <typename T>
struct pending_update {
  std::int64_t at_ms = 0;

  template <typename Archive>
  void serialize(Archive& ar) {
    ar(at_ms);
  }
};

// ---------------------------------------------------------------------------
// link_state — public enum the app can read to drive a connection status
// indicator. Maintained on the global entity by sync_client / app code.
// Phase 5 of the offline-first plan.
// ---------------------------------------------------------------------------
enum class link_state {
  offline,    // not connected and not currently attempting (no config or after a final failure)
  connecting, // socket up, handshake in flight
  reconciling,// pending changes uploading
  syncing,    // initial snapshot from server arriving
  online,     // steady state — connected, snapshot loaded, no pending work
};

} // namespace entt_ext::sync
