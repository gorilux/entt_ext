#pragma once

#include <entt_ext/ecs.hpp>
#include <entt_ext/entity_mapping.hpp>
#include <entt_ext/sync/pending_changes.hpp>
#include <entt_ext/sync_common.hpp>
#include <entt_ext/type_name.hpp>

#include <grlx/rpc/client.hpp>
#include <grlx/rpc/encoder.hpp>
#include <grlx/rpc/message.hpp>
#include <grlx/rpc/tcp_channel.hpp>

// Cereal archive header is needed because the inline
// load_component_and_hierarchy<T> helper takes a
// cereal::PortableBinaryInputArchive& in its signature. Heavier cereal
// type-includes and spdlog only used by function bodies live in
// sync_client_impl.hpp, which this header auto-includes at the bottom
// unless ENTT_EXT_SYNC_CLIENT_NO_IMPL is defined by the build system.
#include <cereal/archives/portable_binary.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <chrono>
#include <string>
#include <vector>

namespace entt_ext::sync {

namespace asio = boost::asio;

// Client-side synchronization manager.
//
// Mirrors sync_server: parameterized over the grlx-rpc channel type. The
// historical `sync_client<Components...>` name is preserved below as a
// default-tcp_channel alias so existing apps keep compiling.
//
// Member function bodies live in sync_client_impl.hpp. Tiny inlines
// (getters, simple delegations, hierarchy mapping helpers without
// spdlog) remain in this header. Heavy bodies (anything using spdlog,
// cereal types, with non-trivial lambdas) are out-of-line so TUs that
// just need the type don't pay parse cost for them.
template <typename ChannelT, typename... SyncComponentsT>
class sync_client_with_channel {
  using channel_type = ChannelT;
  using rpc_client   = grlx::rpc::client<channel_type>;
  using tcp          = boost::asio::ip::tcp;

public:
  template <typename... ChannelArgs>
  explicit sync_client_with_channel(ecs& ecs_instance, ChannelArgs&&... channel_args);

  // Connect to the sync server and perform handshake
  asio::awaitable<bool> connect(std::string const& host,
                                std::uint16_t      port,
                                std::string const& client_name    = "",
                                std::string const& client_version = "",
                                std::string const& username       = "",
                                std::string const& password       = "");

  // Auth info from last successful handshake
  int auth_role() const {
    return auth_role_;
  }
  std::string const& auth_token() const {
    return auth_token_;
  }
  std::string const& handshake_error() const {
    return handshake_error_;
  }

  // Disconnect from the sync server.
  //
  // When `clear_mapping` is true, also destroys every locally-mapped entity
  // and drops the continuous_loader's remote↔local maps. Use this on a
  // reconnect path where stale server-entity IDs would otherwise misroute
  // outbound updates — clicks against an entity whose server counterpart
  // changed identity (server restart, session expired and re-issued) end up
  // hitting the wrong (or missing) server entity. Cost is a brief UI flicker
  // while the next snapshot rebuilds the world, plus loss of any
  // pending_create<T> / pending_update<T> markers (they live on the
  // entities being destroyed) — so do NOT pass true if you rely on
  // offline-first reconciliation across this disconnect.
  asio::awaitable<void> disconnect(bool clear_mapping = false) {
    try {
      rpc_client_.disconnect();
    } catch (...) {
      // Ignore disconnect errors
    }
    session_id_.clear(); // Clear session on disconnect

    if (clear_mapping) {
      // Snapshot first, then clear the maps, *then* destroy. The component
      // and entity on_destroy observers consult continuous_loader_ /
      // loading_snapshot_ to decide whether to send removal RPCs; with
      // maps already empty and the suppression flag set, both short-circuit
      // — which is what we want, since we're already disconnected.
      auto locals = continuous_loader_.local_entities();
      continuous_loader_.clear_mappings();

      loading_snapshot_ = true;
      for (auto e : locals) {
        if (ecs_.valid(e)) {
          ecs_.destroy(e);
        }
      }
      loading_snapshot_ = false;
    }

    co_return;
  }

  // Check if connected to server
  bool is_connected() const {
    return rpc_client_.is_connected();
  }

  // Check if we have a valid session
  bool has_session() const {
    return !session_id_.empty();
  }

  // Get the current session ID
  std::string const& get_session_id() const {
    return session_id_;
  }

  // Get server entity for a client entity (returns null if not mapped)
  entity get_server_entity(entity client_entity) const {
    return continuous_loader_.to_remote(client_entity);
  }

  // Get client entity for a server entity (returns null if not mapped)
  entity get_client_entity(entity server_entity) const {
    return continuous_loader_.map(server_entity);
  }

  // Check if entity is mapped to server
  bool is_entity_mapped(entity server_entity) const {
    return continuous_loader_.contains(server_entity);
  }

  // Persist the server→client entity-id mapping. Used by offline-first
  // sync clients that cache the registry across runs (see
  // docs/offline_first.md). Without this, restoring the registry from
  // disk is not enough — the next server snapshot would re-map server
  // entity IDs to fresh client entities and duplicate everything.
  template <typename Archive>
  void save_mapping(Archive& archive) const {
    continuous_loader_.save_mapping(archive);
  }

  // Inverse of save_mapping. Must be called *after* the registry has
  // been restored from its own snapshot — the loader drops mapping
  // entries whose local entity isn't valid.
  template <typename Archive>
  void load_mapping(Archive& archive) {
    continuous_loader_.load_mapping(archive);
  }

  // Offline-first registry cache (see docs/offline_first.md).
  //
  // The cache file is a *server-keyed* snapshot: byte-for-byte the same
  // shape the server emits in a live sync_response. save_cached_snapshot
  // mirrors the server's build_filtered_registry — it builds a temporary
  // registry whose entity table holds server IDs (translated from the
  // live client IDs via the continuous_loader), so the persisted file is
  // independent of the arbitrary local IDs this run happened to assign.
  // restore_cached_snapshot then replays it through the exact same
  // continuous_loader path used for a live snapshot, so a subsequent
  // server snapshot reuses the restored entities instead of duplicating
  // them. Neither requires an empty registry (unlike entt::snapshot_loader,
  // which EnTT 4.0 hard-asserts — incompatible with entt_ext's always-present
  // global entity).
  void save_cached_snapshot(cereal::PortableBinaryOutputArchive& archive);

  asio::awaitable<void> restore_cached_snapshot(cereal::PortableBinaryInputArchive& archive);

  // Suppress the on_construct/on_update/on_destroy observers that
  // would otherwise try to send component state to the server. Used by
  // offline-first cache loading: emplace events fired while restoring
  // a snapshot from disk are local-only and must not produce RPCs (the
  // session may not even exist yet). Caller is responsible for restoring
  // the previous value via the matching pop. Re-uses the existing
  // `loading_snapshot_` flag the observers already check.
  void push_suppress_observer_rpcs() {
    loading_snapshot_ = true;
  }
  void pop_suppress_observer_rpcs() {
    loading_snapshot_ = false;
  }

  // Request ECS snapshot from server
  asio::awaitable<bool> request_snapshot(std::vector<entity> const& entities_of_interest = {});

  // Sync-level keepalive: invokes "sync_keepalive" on the server to refresh
  // last_sync for our session in client_states_. The rpc-layer try_ping()
  // only resets the rpc msg_reader's idle deadline (5 min) — it does not
  // touch the higher-level sync session (15 min default). For an idle but
  // connected client (e.g. a dashboard left open overnight), only this RPC
  // prevents server-side eviction. Returns false if the session is unknown
  // server-side (caller should reconnect/re-handshake) or on any error.
  asio::awaitable<bool> keepalive();

  // Connect to server and request initial snapshot in one operation
  asio::awaitable<bool> connect_and_sync(std::string const&         host,
                                         std::uint16_t              port,
                                         std::vector<entity> const& entities_of_interest = {},
                                         std::string const&         client_name          = "",
                                         std::string const&         client_version       = "");

  // Force sync all pending changes (useful after reconnection)
  asio::awaitable<void> force_sync_all() {
    // This will cause all systems to process their pending markers
    // when the next frame runs (assuming we're connected)
    co_return;
  }

  // Apply synchronized state from server
  asio::awaitable<bool> apply_sync_response(sync_response const& response);

  // Phase 4: drain any pending_create<T> / pending_update<T> markers
  // accumulated while disconnected and push the corresponding component
  // values to the server via the existing send_component_to_server path.
  // Multi-pass over creates so a hierarchy chain (parent created offline,
  // child created offline) eventually reconciles even if the child's
  // parent<T> ref isn't mapped yet on the first pass — once the parent
  // is sent and acknowledged, continuous_loader.contains_local() flips
  // true for it and the next pass picks the child up.
  asio::awaitable<void> reconcile_pending_changes();

  template <typename First, typename... Rest>
  asio::awaitable<void> reconcile_creates_helper(bool& progress);

  template <typename First, typename... Rest>
  asio::awaitable<void> reconcile_updates_helper();

  template <typename ComponentT>
  asio::awaitable<void> reconcile_creates_for(bool& progress);

  template <typename ComponentT>
  asio::awaitable<void> reconcile_updates_for();

private:
  // Set up notification handlers for real-time sync updates
  void setup_notification_handlers();

  // Set up notification handlers for a specific component type
  template <typename ComponentT>
  void setup_component_notification_handlers();

  // Implementation of notification handler setup for a single component
  template <typename ComponentT>
  void setup_component_notification_handlers_impl();

  // Set up automatic sync for a specific component type
  template <typename ComponentT>
  void setup_automatic_sync();

  // Implementation of automatic sync setup for a single component.
  // ReadOnly=true for server_only components: skip send-to-server observers.
  template <typename ComponentT, bool ReadOnly = false>
  void setup_automatic_sync_impl();

  // Request a server entity for a client entity
  asio::awaitable<entity> request_server_entity(entity client_entity);

  // Send a specific component to the server using type-safe endpoints (insert or update)
  template <typename ComponentT>
  asio::awaitable<void> send_component_to_server(entity e, ComponentT& component, version_type sync_version);

  // Notify server about component removal using type-safe endpoints
  template <typename ComponentT>
  asio::awaitable<void> notify_component_removal(entity e, version_type sync_version);

  // Notify server about entity destruction
  asio::awaitable<void> notify_entity_destruction_to_server(entity e, version_type sync_version);

  // Helper to load component and its hierarchy components from archive
  template <typename ComponentT>
  void load_component_and_hierarchy(cereal::PortableBinaryInputArchive& archive) {
    using ActualT = unwrap_hierarchy_t<ComponentT>;

    // Load the component itself
    continuous_loader_.template get<ActualT>(archive);

    // Also load hierarchy components if wrapped with with_hierarchy<T>
    if constexpr (is_with_hierarchy_v<ComponentT>) {
      continuous_loader_.template get<entt_ext::parent<ActualT>>(archive);
      continuous_loader_.template get<entt_ext::children<ActualT>>(archive);
    }
  }

  // Helper to remap component and its hierarchy components
  template <typename ComponentT>
  asio::awaitable<void> remap_component_and_hierarchy() {
    using ActualT = unwrap_hierarchy_t<ComponentT>;

    // Remap hierarchy components if wrapped with with_hierarchy<T>
    if constexpr (is_with_hierarchy_v<ComponentT>) {
      co_await remap_component_entities<entt_ext::parent<ActualT>>();
      co_await remap_component_entities<entt_ext::children<ActualT>>();
    }
    // Remap entity references inside components marked with with_entity_refs
    if constexpr (is_with_entity_refs_v<ComponentT>) {
      co_await remap_component_entities<ActualT>();
    }
    co_return;
  }

  // Helper to remap entity references in components after loading from snapshot
  template <typename ComponentT>
  asio::awaitable<void> remap_component_entities() {
    auto view = ecs_.view<ComponentT>();
    for (auto entity : view) {
      auto& component = view.template get<ComponentT>(entity);
      co_await map_entities_async(component);
    }
    co_return;
  }

  template <typename Type>
  asio::awaitable<void> map_entities_async(parent<Type>& parent) {
    auto local_entity = continuous_loader_.map(parent.entity);
    if (local_entity == entt_ext::null) {
      local_entity = ecs_.create();
      continuous_loader_.insert_mapping(parent.entity, local_entity);
    }
    parent.entity = local_entity;
    co_return;
  }

  template <typename Type>
  asio::awaitable<void> map_entities_async(children<Type>& child_set) {
    children<Type> mapped_set;
    for (auto child_entity : child_set) {
      auto local_entity = continuous_loader_.map(child_entity);
      if (local_entity == entt_ext::null) {
        local_entity = ecs_.create();
        continuous_loader_.insert_mapping(child_entity, local_entity);
      }
      mapped_set.insert(local_entity);
    }
    child_set.swap(mapped_set);
    co_return;
  }

  // Generic map_entities_async for components with a map_entities member (e.g. automation::targets)
  template <typename ComponentT>
    requires requires(ComponentT& c, continuous_loader_with_mapping<entt::registry> const& l) { c.map_entities(l); } &&
             (!sync::is_hierarchy_component_v<ComponentT>)
  asio::awaitable<void> map_entities_async(ComponentT& component);

  // Helper to map entity references to remote (server) IDs before sending
  // If any referenced entities don't have server mappings, request them first
  template <typename ComponentT>
  asio::awaitable<void> map_component_entities_to_remote_async(parent<ComponentT>& component) {
    // First, ensure all referenced entities have server mappings
    auto server_entity = continuous_loader_.to_remote(component.entity);
    if (server_entity == entt_ext::null) {
      server_entity = co_await request_server_entity(component.entity);
      continuous_loader_.insert_mapping(server_entity, component.entity);
    }
    component.entity = server_entity;
    co_return;
  }

  template <typename ComponentT>
  asio::awaitable<void> map_component_entities_to_remote_async(children<ComponentT>& component) {
    children<ComponentT> mapped_set;
    for (auto child_entity : component) {
      auto server_entity = continuous_loader_.to_remote(child_entity);
      if (server_entity == entt_ext::null) {
        server_entity = co_await request_server_entity(child_entity);
        continuous_loader_.insert_mapping(server_entity, child_entity);
      }
      mapped_set.insert(server_entity);
    }
    component.swap(mapped_set);
    co_return;
  }

  // Generic remote mapping for components with map_entities_to_remote member (e.g. automation::targets)
  template <typename ComponentT>
    requires requires(ComponentT& c, continuous_loader_with_mapping<entt::registry> const& l) { c.map_entities_to_remote(l); } &&
             (!sync::is_hierarchy_component_v<ComponentT>)
  asio::awaitable<void> map_component_entities_to_remote_async(ComponentT& component) {
    // Ensure all referenced local entities have server mappings before converting
    if constexpr (requires(ComponentT const& c) { c.entity_refs(); }) {
      for (auto local_entity : component.entity_refs()) {
        if (local_entity != entt_ext::null && continuous_loader_.to_remote(local_entity) == entt_ext::null) {
          auto server_entity = co_await request_server_entity(local_entity);
          if (server_entity != entt_ext::null) {
            continuous_loader_.insert_mapping(server_entity, local_entity);
          }
        }
      }
    }
    component.map_entities_to_remote(continuous_loader_);
    co_return;
  }

  // Perform handshake with server to get session ID (includes authentication)
  asio::awaitable<bool> perform_handshake(std::string const& client_name,
                                          std::string const& client_version,
                                          std::string const& username = "",
                                          std::string const& password = "");

  void setup_entity_sync();

  // Shared restore path: load the entity table + every sync component
  // (and hierarchy) from a snapshot archive through the continuous_loader,
  // drop orphans, then remap entity references. Used by both the live
  // apply_sync_response and the offline-first restore_cached_snapshot so
  // there is exactly one snapshot-ingest implementation.
  asio::awaitable<void> load_snapshot_from_archive(cereal::PortableBinaryInputArchive& archive);

  // Copy one sync component (and its hierarchy parent<T>/children<T>)
  // for the given mapped entities into a server-keyed temporary registry,
  // translating every entity reference local→server and dropping refs
  // that have no server mapping. Client-side mirror of the server's
  // build_filtered_registry copy_one lambda.
  template <typename ComponentT>
  void copy_component_to_server_keyed(entt::registry&            tmp,
                                      std::vector<entity> const& mapped_local,
                                      std::vector<entity> const& mapped_server);

public:
  // Access the underlying RPC client (e.g. to register custom notification handlers)
  rpc_client& get_rpc_client() {
    return rpc_client_;
  }

private:
  // Declaration order matches the constructor initializer list; C++ initializes
  // members in declaration order regardless of the list, so these must agree.
  ecs&                                           ecs_;
  std::string                                    protocol_version_; // Protocol version based on component types
  continuous_loader_with_mapping<entt::registry> continuous_loader_;
  rpc_client                                     rpc_client_;
  std::string                                    session_id_;       // Session ID obtained from handshake
  std::string                                    handshake_error_;  // Last handshake error message
  int                                            auth_role_ = 0;    // Role from auth (0=user, 1=admin)
  std::string                                    auth_token_;       // Auth token from handshake
  bool                                           loading_snapshot_ = false;
};

// Backward-compatible alias — preserves `sync_client<Components...>` for
// existing callers that want plain TCP. For mTLS, use
// sync_client_with_channel<ssl_channel<...>, Components...> directly.
template <typename... SyncComponentsT>
using sync_client = sync_client_with_channel<grlx::rpc::tcp_channel<grlx::rpc::binary_encoder>, SyncComponentsT...>;

} // namespace entt_ext::sync

// Auto-include the implementation by default. Apps that want to opt into
// the lean header (e.g. to centralize codegen via explicit instantiation)
// can define ENTT_EXT_SYNC_CLIENT_NO_IMPL in their build system and pull
// in sync_client_impl.hpp only from a single per-app instantiation TU.
#if !defined(ENTT_EXT_SYNC_CLIENT_NO_IMPL)
#  include <entt_ext/sync_client_impl.hpp>
#endif
