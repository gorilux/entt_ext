# Offline-first sync (design)

Status: design — phase 0 / 5. No implementation has shipped under this
section yet. See `sync_client.hpp` / `sync_server.hpp` for the existing
online-only protocol that this work extends rather than replaces.

## Goal

A client should be able to:

1. Boot with no network, see its previously-synced state.
2. Mutate that state offline (create plans, log workout sets, edit
   exercises) and have those mutations survive a process restart.
3. Reconnect and have offline mutations propagate to the server, while
   accepting any concurrent changes the server received from other
   clients, with predictable conflict resolution.

This is the "Dropbox" / "Notion offline" pattern. It belongs in
`entt_ext::sync` so other apps (`nexus`, `cnc`, …) can opt in by listing
their components for persistence and choosing a conflict policy.

## Non-goals

- Multi-master replication. The server remains the authority; clients
  reconcile *with* the server, not with each other directly.
- Strong consistency. Last-write-wins by wall-clock timestamp is the
  default policy.
- Partial-graph sync. The client keeps a full local copy of every
  synced component or none of it (per-component opt-in is fine, but
  there's no "I only want some entities" facility).

## Why the current code can't do this as-is

The current sync flow on `sync_client::connect`:

1. `request_snapshot()` is called.
2. Server pushes a full snapshot of every synced component.
3. `continuous_loader_with_mapping::get<entity>(archive)` populates a
   *fresh* mapping from server entity IDs to newly-created client entity
   IDs (`remloc_` and `locrem_`).
4. Each component is loaded into the new client entities.
5. `continuous_loader_.orphans()` cleans up orphaned client entities.
6. Real-time updates flow as notifications until disconnect.

If we naively persist client entities to disk and load them at startup,
on the next connect the server snapshot creates a *second* set of
entities with fresh mapping, and the client ends up with duplicates. The
mapping is the missing piece, plus an explicit semantics for "this
client has uncommitted changes; reconcile before accepting the snapshot".

## Proposed protocol additions

### A. Persistable entity-id mapping

`continuous_loader_with_mapping` gains:

```cpp
template <typename Archive>
void save_mapping(Archive& ar) const;

template <typename Archive>
void load_mapping(Archive& ar);
```

These serialize the `remloc_` map. `load_mapping` skips entries whose
local entity is no longer valid (the persisted snapshot may have been
trimmed). Required because `remloc_` is the only thing that lets us
rebind a server-pushed snapshot to existing client entities instead of
duplicating them.

`sync_client` exposes `save_mapping(ar)` / `load_mapping(ar)` thin
wrappers.

### B. Client-side state persistence

A new opt-in module along the lines of the existing
`entt_ext::settings_collection` framework. Apps list which sync
components should be cached locally:

```cpp
using gym_offline_state =
    entt_ext::sync::client_state_collection<base_version,
        entt_ext::persistent_data_model<GYM_SYNC_COMPONENTS_V2>>;
```

The module:

- On startup: deserializes the registry and the loader mapping.
- On every `on_construct` / `on_update` / `on_destroy` for the listed
  components: stamps a `dirty_flag` and triggers `auto_save` (existing
  pattern from `gym::app::module`).

### C. Pending-change tracking

While `sync_client.is_connected() == false`, the client's component
observers no longer attempt RPCs (they currently `co_await` and will
fail with errors that fill the log). Instead they set:

```cpp
template <typename T> struct pending_create {};
template <typename T> struct pending_update { std::int64_t at_ms; };
template <typename T> struct pending_delete { std::int64_t at_ms; };
```

These tags are persisted alongside the component itself.

When the client reconnects, before issuing `request_snapshot`, it
iterates entities carrying any `pending_*<T>` and pushes them via a new
RPC verb (see D).

### D. Reconciliation RPC

New verb: `reconcile`. Payload:

```cpp
struct reconcile_request {
  std::string             session_id;
  std::vector<entity>     deletes;            // server entity IDs
  std::vector<creation>   creates;            // client temp ID + components
  std::vector<update>     updates;            // server entity ID + components + timestamp
};

struct reconcile_response {
  std::vector<id_mapping> created;            // client temp ID -> server entity ID
  std::vector<conflict>   conflicts;          // entries the server refused
};
```

The server applies creates first (assigning new entity IDs and including
hierarchy parent/child wiring), then updates (subject to conflict
policy), then deletes. It returns the new IDs so the client can update
its mapping.

After reconcile completes, the client issues the existing
`request_snapshot` and processes the response normally — but now
`continuous_loader_` already has the right mapping for everything it
just pushed up, plus whatever was previously cached.

### E. Per-component conflict policy

Today `with_hierarchy<T>` and `server_only<T>` decorate the sync list.
We add:

```cpp
namespace entt_ext::sync {

enum class conflict_policy { client_wins, server_wins, last_write_wins };

template <typename T, conflict_policy P = conflict_policy::last_write_wins>
struct with_conflict_policy { using type = T; };

}
```

`last_write_wins` requires components to expose an `updated_at_ms`
field, or a sibling `last_modified_at<T>` component the system stamps.

### F. Connection state visible to the app

`sync_client` already has `is_connected()`. We add an enum exposed via a
new component / accessor:

```cpp
enum class link_state {
  offline,       // never connected, or disconnected
  connecting,    // socket up, handshake in flight
  reconciling,   // pending changes uploading
  syncing,       // snapshot loading
  online,        // steady state
};
```

GUIs that want to surface this to the user can read it; nothing forces
an opinion on the UX.

## Migration path for existing apps

Apps that don't list components in the new persistence collection see no
behavior change — the online-only protocol still works exactly as
before.

Opting in is, in order:

1. List the components you want cached locally in
   `client_state_collection<...>`.
2. Add the persistence module to the client's `init_modules`.
3. (Optional) Wrap server-only-authoritative components in
   `server_wins<T>`; wrap client-only-authoritative ones in
   `client_wins<T>`.
4. (Optional) Surface `link_state` somewhere in the UI.

Nexus / cnc don't need to change unless they want offline support;
their sync surface is unchanged.

## Phasing

1. **Phase 1** (small): A. mapping save/restore on
   `continuous_loader_with_mapping` + `sync_client`.
2. **Phase 2** (medium): B. client-state collection + auto-save +
   load-on-startup. Client can now boot offline and see cached state;
   offline mutations are visible locally but lost on reconnect (server
   snapshot overwrites them).
3. **Phase 3** (medium): C. pending-change tracking. Offline mutations
   survive reconnect-without-server-side-apply.
4. **Phase 4** (large): D + E. reconcile RPC and conflict policies.
   Offline mutations now propagate to server.
5. **Phase 5** (gym wiring): wire all of the above into the gym client,
   add `link_state` UI affordance, integration test.

Each phase is independently shippable and useful even without the next.

## Follow-ups (post-phase-4 v1)

These are tracked here rather than in an in-session TODO list because
they outlive any one editing pass. Phase numbers refer to the phasing
section above; "follow-up" items below are deltas on the v1 phase that
already shipped.

### Phase 2 follow-up: server-keyed cache (shipped)

The original phase-2 `client_state_cache` snapshotted the **live client
registry verbatim** (`entt::snapshot` of local IDs) and restored it with
`entt::snapshot_loader`, then layered `save_mapping`/`load_mapping` on
top. EnTT 4.0's `basic_snapshot_loader` constructor hard-asserts
`registry.storage<entity>().free_list() == 0` ("Registry must be
empty"), and `entt_ext::ecs` always owns a global entity (plus any
modules imported before the sync client) — so the loader aborts on
startup the moment a cache file exists.

The fix realigns the implementation with the design above: the cache
file is now a **server-keyed snapshot**, byte-identical to a live
`sync_response.snapshot_data`.

- `sync_client::save_cached_snapshot` mirrors the server's
  `build_filtered_registry`: it builds a temporary registry whose
  entity table holds *server* IDs (translated from local IDs via the
  continuous_loader, dropping refs with no server mapping), then
  `entt::snapshot`s that.
- `sync_client::restore_cached_snapshot` replays the file through the
  **same** `continuous_loader_` ingest path as a live snapshot
  (`load_snapshot_from_archive`, shared with `apply_sync_response`), so
  no empty-registry precondition applies and a subsequent server
  snapshot reuses the restored entities instead of duplicating them.

The persisted entity-id mapping is now implicit in the server-keyed
table (the continuous_loader rebuilds it on restore), so
`client_state_cache` no longer calls `save_mapping`/`load_mapping` — the
thin `sync_client` wrappers remain for any external caller. Offline-only
entities (no server ID yet) are still not cached; that remains the
documented phase-2 limitation closed by phase 3/4.

### Phase 4 follow-up: pending_delete tombstones

Today an entity destroyed offline cannot be propagated to the server —
the dying entity can't carry a `pending_delete<T>` marker because all
its components are gone by the time observers run. The fix is a global
tombstone list:

```cpp
struct pending_deletes {
  struct entry {
    entt_ext::entity server_entity;  // mapped server-side ID at time of delete
    std::int64_t     at_ms;
  };
  std::vector<entry> entries;
};
```

Stamped on the global entity by sync_client's `on_destroy` observer
(which translates the local-entity-being-destroyed to its server ID via
`continuous_loader_.to_remote(e)` BEFORE the entity is gone). Persisted
by client_state_cache. Drained by reconcile via the existing
`notify_component_removal` path. Without this, deleting a plan offline
still shows it gone locally but it reappears on reconnect when the
server snapshot lands.

### Phase 4 follow-up: per-component conflict policy

Today reconcile is "client wins". For multi-device setups this isn't
right — a phone reconciling stale offline edits will overwrite a newer
edit a desktop client made while the phone was at the gym.

The protocol-side change in the design proposal:

```cpp
namespace entt_ext::sync {
enum class conflict_policy { client_wins, server_wins, last_write_wins };

template <typename T, conflict_policy P = conflict_policy::last_write_wins>
struct with_conflict_policy { using type = T; };
}
```

The server-side change: when applying a `component_update_request`,
compare the incoming `pending_update<T>::at_ms` (or
`pending_create<T>` synthesized timestamp) against the
`server_last_modified_at` it stamps on its own copy. Apply only if the
incoming timestamp is newer (for `last_write_wins`) or always
(`client_wins`) or never (`server_wins`).

For the gym single-user case `client_wins` is fine and is what we
have. Add the policy hooks before recommending nexus / cnc adopt
offline-first.

### Phase 4 follow-up: batched reconcile RPC

Today reconcile sends one component-per-RPC. For users with hundreds of
queued sets logged across multiple offline gym sessions, that's a
noticeable reconnect delay. The original design called for a single
`reconcile` RPC carrying all pending creates + updates + deletes in one
message, with the server returning the temp→server ID mapping in one
response.

Worth doing once observability tells us the per-component round-trip
cost actually matters; until then the simpler reuse-of-existing-path
implementation is fine.

### Phase 4 follow-up: hierarchy ordering scale

The current creates loop runs at most 16 passes. Pathologically wide
hierarchies (think 17+ levels of plan→split→day→exercise→…) would
silently fail to reconcile. Replace with a topological sort if/when an
app actually trips this. Today nexus, cnc, gym all have 1–3 levels.

### Phase 4 follow-up: clock skew and last-write-wins

Phone clocks can drift; if a drifted client wins a conflict it
shouldn't have, the user sees data loss. Mitigate by stamping at_ms on
the server's clock when first persisting and treating client at_ms as a
monotonic-only ordering hint (server retains its copy as the
authoritative timestamp).

## Open questions

- **Hierarchy creation order on reconcile.** When the client uploads a
  newly-created entity that has `parent<X>` referring to another
  client-temp ID, the server must resolve the parent ID first. The
  current plan is "client creates parents before children in the
  request payload"; if that fails we'll need a two-pass apply.
- **Large pending queues.** If the client is offline for weeks and
  generates thousands of changes, the reconcile payload may exceed the
  message size cap (`session_limits::max_message_bytes`, 8 MiB). We
  may need chunked reconciliation; left for follow-up.
- **Clock skew and last-write-wins.** Phone clocks can drift; if a
  drifted client wins a conflict it shouldn't have, the user sees data
  loss. We can mitigate by having the server stamp `at_ms` with its own
  clock when first persisting and treating client `at_ms` as a
  monotonic-only ordering hint.
