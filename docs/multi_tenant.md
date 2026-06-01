# Multi-tenant sync (design)

Status: design — phase A of N. No implementation has shipped under this
section yet. Builds on the offline-first work (`offline_first.md`).

## Goal

A single `entt_ext::sync::sync_server` can serve multiple authenticated
users, with each user only seeing and modifying *their own* entities.
Anonymous (no auth) deployments stay supported as a degenerate case
where every session has `user_id = ""` and every entity has `owner = ""`
— effectively single-tenant.

## What's already in place

- **Protocol-level auth.** `entt_ext::sync::handshake_request` already
  carries `username` + `password`; `handshake_response` carries
  `session_id` + `role` + `auth_token`. The server's
  `set_auth_handler` lets an app validate credentials.
- **Reference implementation.** `apps/nexus/server/src/modules/auth/`
  has the full thing: `user_account` component, PBKDF2 password
  hashing, rate limiting, token store, admin user-management RPCs.
  Not in entt_ext today; it lives in nexus.

## What's missing — and what this design adds

1. **`entt_ext::sync::owner`** — a server-only component carrying the
   `user_id` of an entity's creator. Stamped automatically by the
   server when a session creates a new entity.
2. **Per-session user identity tracked in sync_server.** Today the
   handshake validates credentials but the server doesn't remember the
   resulting `user_id` against the `session_id` for sync-time use. Add a
   `session_id → session_identity{user_id, role}` map.
3. **Owner-filtered snapshot.** When a session calls `request_snapshot`,
   serialize only entities whose `owner == session.user_id`, plus
   entities marked as global (`owner == ""`).
4. **Owner-filtered notifications.** When the server broadcasts a
   component update, only deliver to sessions whose `user_id` matches
   the entity's `owner`.
5. **Write authorization.** Reject `component_update_request` /
   `component_remove_request` for entities whose `owner != session.user_id`,
   unless the session role is `admin` or the entity is unowned.
6. **Hierarchy ownership inheritance.** `parent<T>` / `children<T>`
   relationships propagate the owner: when a child is created via
   `emplace_child`, the child inherits the parent's owner if the
   creating session doesn't have a different one. (For the single-user
   anonymous case this is a no-op since both are `""`.)

## Promotion of nexus auth into entt_ext

The auth_module pieces are general-purpose. Move into entt_ext so other
apps (gym, cnc) get them for free:

- `entt_ext::sync::user_account` (component): username, password_hash,
  salt, role, created_at.
- `entt_ext::sync::password::{hash,verify,needs_rehash}` (helpers):
  PBKDF2 wrapper.
- `entt_ext::sync::login_rate_limiter`.
- `entt_ext::sync::token_store`.
- `entt_ext::sync::auth_module` (helper module): `set_auth_handler`
  wired against persisted `user_account` components.

Nexus's existing auth_module becomes a thin wrapper around the moved
implementation; nothing observable changes for nexus.

## Data model

```cpp
namespace entt_ext::sync {

// Server-only — never shipped to clients via the snapshot path.
// Stamped on every entity that has a user-owned synced component.
struct owner {
  std::string user_id;
};

// Per-session identity, attached internally by sync_server after
// successful handshake. Not exposed via sync.
struct session_identity {
  std::string user_id;
  int         role;
};

}
```

`server_only<owner>` is implicit — owner is already filtered out of
client snapshots because it's wrapped in `server_only<>` in the
internal sync list the server builds. Apps don't need to know about it.

## Filtering rules

When generating a snapshot for a session with `user_id = U`:

```
include entity E iff
  E has no owner component OR owner(E).user_id == U
```

When broadcasting a notification for an update on entity E with
`owner = O`:

```
deliver to session S iff
  S.user_id == O OR S.role == admin OR O is empty (unowned)
```

When applying a `component_update_request` on entity E from session S:

```
allow iff
  E is new OR owner(E).user_id == S.user_id OR S.role == admin
on first allow for a new entity:
  emplace<owner>(E, {S.user_id})
```

## Anonymous / no-auth case

If `auth_handler_` is not set, the server treats every session as
`user_id = ""`. Every entity gets `owner = ""`. Every session sees
every entity. This matches today's single-user behavior. The
multi-tenant code paths are no-ops in this configuration.

## Non-goals

- **Cross-tenant sharing.** "User A shares plan X with user B" is out
  of scope. Could be added later via an explicit `shared_with` set on
  the entity, but it's a different feature.
- **Row-level RBAC beyond owner.** No "this entity is editable by user
  B but visible to user C" — owner is a single user_id. Roles only
  give the special-cased admin override.
- **Federated multi-server.** A server is one tenant pool; users on
  server X can't see anything on server Y. Mirroring multiple servers
  is a separate concern from multi-tenancy within one server.

## Phasing

Each phase is independently shippable.

**Phase A — server-side ownership primitives.**

*Phase A v1 — landed.* `owner` component at
`subprojects/entt_ext/include/entt_ext/sync/owner.hpp`.
`client_sync_state.user_id` + `.role` populated from
`auth_handler` result during handshake (`sync_server.hpp`). Snapshot
filtering via `collect_visible_entities` + iterator-range
`entt::snapshot::get`. Owner stamping on first write to an entity in
`update_request_observer.on_construct`. Write authorization rejects
`component_update_request` for entities whose owner doesn't match
the session's user_id (role==1 admin bypasses). Anonymous (no
`auth_handler`) deployments are unaffected: every session has
`user_id = ""` and every entity gets `owner = ""`, so the filter
passes everything.

*Phase A v1 known gaps — must be closed in v2:*

- **Notifications still broadcast.** `rpc_server_.notify(...)` fans
  every component update out to every connected session. The
  receiving client filters by `request.session_id` but the bytes were
  already sent on the wire. The primitives to fix this are now in
  place — `grlx::rpc::session::logical_session_id_` and
  `grlx::rpc::server::notify_session(target_id, ...)` — but
  sync_server's notify paths still call the broadcast `notify(...)`
  because the handshake handler doesn't currently have access to the
  underlying `session*` to call `set_logical_session_id` on it. Phase
  A v2 needs an `attach_with_session<>(name, callback)`-style hook on
  rpc_server so the handshake handler gets the session pointer and
  stamps the generated session_id on it. Then
  `notify_component_update_to_other_clients` etc. switch from
  `notify(...)` to `notify_session(client_id, ...)` and only deliver
  bytes to the right tenant.
- **No hierarchy ownership inheritance.** A child entity created
  under a parent owned by user A doesn't auto-inherit `owner = A`.
  Today the child is stamped with the creator's user_id, which
  happens to be A in the typical case (same session that created the
  parent), but a different session creating a child under A's parent
  would stamp itself as owner — wrong. Fix in v2: in
  `update_request_observer` for `parent<T>`, copy the parent
  entity's `owner` onto the child if the child is unowned.
- **Entity table itself isn't filtered.** `entt::snapshot::get<entity>`
  has no iterator-range overload, so the entity-id table on the wire
  contains every entity in the registry — components are filtered out
  but the entity ids arrive on the client as orphans and
  `continuous_loader_.orphans()` reaps them. Information leak: count
  and entity-id range of other tenants' data. Acceptable for the
  home-server case; stricter isolation needs a temporary filtered
  registry.

**Phase A v2 — landed.** Closed the major v1 gaps:

- *Notification routing.* `grlx::rpc::client_context` now carries a
  `set_logical_session_id` callback bound to the calling session.
  `grlx::rpc::current_call_context()` exposes that ctx as a thread-local
  to handlers for the synchronous prefix of their coroutine. The
  `current_call_context_scope` RAII inside `session::dispatch_request`
  installs/clears it. sync_server's handshake reads the ctx, calls
  `set_logical_session_id(generated_session_id)`, and now both
  `notify_component_update_to_other_clients` and
  `notify_component_removal_to_other_clients` use
  `rpc_server.notify_session(client_id, ...)` filtered by
  `is_entity_visible_to(...)`. Bytes for user A's entities never leave
  the server bound for user B's sockets.
- *Hierarchy owner inheritance.* `is_parent_v<T>` trait added in
  `entt_ext/ecs.hpp`. `update_request_observer.on_construct` now copies
  the parent entity's `owner` onto the child if the child has no owner
  (or its `user_id` is empty), so a snapshot for user A correctly
  includes every descendant of an A-owned root.

*Phase A v2 still-open follow-ups:*

- ~~Entity table itself isn't filtered.~~ **Closed.** `sync_server`
  now constructs a per-request temporary `entt::registry` containing
  only visible entities (with their actual server IDs preserved via
  `create(hint)`) and snapshots that registry. `parent<T>` refs and
  `children<T>` sets that point at not-visible entities are dropped
  during the copy. Implemented as `build_filtered_registry<...>` and
  `save_component_and_hierarchy_from(reg, archive)` in `sync_server.hpp`.
  The wire is now free of cross-tenant entity ids.
- Handlers must read `current_call_context()` synchronously (before
  the first `co_await`); after a suspension the thread-local is
  unreliable. For our handshake handler that's fine; future ctx-using
  handlers must follow the same rule or capture what they need.

**Phase A complete.** Server-side multi-tenant primitives are now fully
in place: ownership, snapshot filtering (entity table + components +
hierarchy), per-tenant notification routing, write authorization,
hierarchy owner inheritance. The remaining gap is real auth — the
user_id strings flowing through the protocol are still unvalidated
because the gym app hasn't installed an auth_handler yet. Phase B
promotes nexus's auth into entt_ext to make user_ids meaningful.

## Compilation-time optimization (separate concern)

The header-only nature of `sync_server.hpp` / `sync_client.hpp` /
`client_state_cache.hpp` means touching any of them recompiles every TU
that includes them — gym, nexus, cnc all rebuild. Combined with the
per-(component × operation) template instantiation count, cold builds
are slow. Two structural refactors would materially help:

1. **Move per-component sync template instantiations into per-app .cpp
   files** with `extern template` declarations in the headers, rather
   than re-emitting them in every TU. Same pattern entt itself uses
   for `basic_registry`. Cuts redundant template work across TUs.
2. **Split `sync_server.hpp` / `sync_client.hpp` into a stable header
   (declarations) + an impl header (definitions) included only by
   per-app instantiation .cpp files.** Lets app authors `extern
   template` the few sync_server/sync_client specializations they
   actually use.

Not blocking for phase B/C/D, but the iteration cost is real. Worth
prioritizing once auth + gym wiring are stable enough that the
sync_server.hpp surface stops churning.

**Phase B — promote nexus auth into entt_ext.** Move `user_account`,
`password`, `login_rate_limiter`, `token_store`, `auth_module` from
nexus to entt_ext. Nexus keeps working through a thin shim.

**Phase C — wire gym for multi-user.**
- `gym-server` imports `entt_ext::sync::auth_module`.
- Gym client adds username + password fields to its config (mirror
  nexus's `config::save/load`).
- Login screen on first launch.
- Bootstrap admin via env / first-run flow.

**Phase D — admin role + user management.** *(done)*
- User-management RPCs (`auth_create_user`, `auth_delete_user`,
  `auth_change_password`, `auth_list_users`) registered by
  `entt_ext::sync::register_user_management_endpoints`. Naming aligned
  with nexus (underscored, not dotted) so the same UI code works on
  both apps.
- Gym `user_management_dialog` (admin-only) — gear icon next to the
  link-state badge opens a floating window with the user list, a
  "Create user" form and a per-user "Change password" form.
- `gym::ecs_sync::rpc_client_ref` exposed on the global entity so the
  dialog can issue RPCs without holding a `client_module` reference.

**Phase E — hardening.** *(in progress)*
- Rate limiting was already integrated in phase B
  (`entt_ext::sync::login_rate_limiter`, called from
  `auth_module::auth_handler`).
- Token rotation: gym client now refreshes its token every 6h while
  connected (`auth_refresh` RPC; server TTL defaults to 24h, so we
  have 4× headroom). The new token replaces both
  `connection_state.auth_token` (live) and `config.auth_token`
  (persisted).
- TODO: tests for "user A cannot read user B's data" — defer until the
  test scaffolding for entt_ext::sync exists (currently apps test
  against a real server).

## Migration

Existing single-user deployments keep working with no changes. A user
adopting auth runs:

1. Set `auth_handler_` (typically by importing `auth_module`).
2. Migrate any pre-existing entities by stamping `owner = "<bootstrap_user>"`
   on them once.

After that the existing data belongs to the bootstrap user and
multi-tenant behavior takes over.

## Threat model not covered

- **Brute-force protection** lives in the rate limiter (phase E). Not
  this design.
- **Replay attacks** on the auth_token are out of scope for this
  design; the existing token_store with TTLs in nexus is the answer.
- **TLS** is already mandatory (the `ssl_channel` channel type).
