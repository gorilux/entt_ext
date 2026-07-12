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
// Entity-level deletes can't carry a pending_delete<T> marker the same way
// (the dying entity's components are gone by the time on_destroy observers
// run), so they use a separate tombstone list on the *global* entity —
// see pending_deletes below.

#include <entt_ext/core.hpp>

#include <cstdint>
#include <vector>

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
// pending_deletes — phase 4 follow-up (see docs/offline_first.md). Lives on
// the global entity, not the dying one: sync_client's entity-destroy
// observer resolves the entity's server id (continuous_loader_.to_remote)
// before the mapping is dropped and appends an entry here whenever the
// entity_destroy RPC can't go out immediately (offline, or the RPC itself
// failed). Persisted by client_state_cache alongside everything else so the
// delete survives a process restart; drained by
// sync_client::reconcile_pending_deletes on the next connect, which retries
// the entity_destroy RPC per entry and removes it on success.
// ---------------------------------------------------------------------------
struct pending_deletes {
  struct entry {
    entt_ext::entity server_entity = entt_ext::null;
    std::int64_t     at_ms         = 0;

    template <typename Archive>
    void serialize(Archive& ar) {
      ar(server_entity, at_ms);
    }
  };

  std::vector<entry> entries;
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
