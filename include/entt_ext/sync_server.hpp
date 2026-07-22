#pragma once

#include <entt_ext/ecs.hpp>
#include <entt_ext/entity_mapping.hpp>
#include <entt_ext/sync/owner.hpp>
#include <entt_ext/sync_common.hpp>
#include <entt_ext/type_name.hpp>

#include <grlx/rpc/encoder.hpp>
#include <grlx/rpc/message.hpp>
#include <grlx/rpc/server.hpp>
#include <grlx/rpc/tcp_channel.hpp>

#include <entt/entity/snapshot.hpp>

// Cereal archive header is needed because cereal::PortableBinaryOutputArchive
// appears in private member function signatures (save_component_and_hierarchy*).
// The cereal/types/* and spdlog includes that only function bodies need are
// deferred to sync_server_impl.hpp, which this header auto-includes at the
// bottom unless ENTT_EXT_SYNC_SERVER_NO_IMPL is defined by the build system.
#include <cereal/archives/portable_binary.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <algorithm>
#include <chrono>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace entt_ext::sync {

namespace asio = boost::asio;

// Per-client sync state
struct client_sync_state {
  std::chrono::steady_clock::time_point last_sync;       // Updated on every RPC access; basis for stale-client eviction
  std::chrono::steady_clock::time_point last_push;
  std::unordered_set<entity>            dirty_entities;  // Entities that changed since last sync to this client
  std::string                           client_id;
  bool                                  full_resync_needed = false; // Set when dirty_entities was capped, forcing the client to re-snapshot

  // Multi-tenant identity (see docs/multi_tenant.md). Set during the
  // handshake from the auth_handler's response. Empty user_id ("") =
  // anonymous / single-tenant mode — every entity is visible.
  std::string user_id;
  int         role = 0; // 0 = user, 1 = admin (matches handshake_response::role)

  template <typename Archive>
  void serialize(Archive& archive) {
    archive(last_sync, last_push, dirty_entities, client_id);
  }
};

// Hard cap on dirty_entities per client. A wedged client whose sync calls
// have stopped landing must not be allowed to consume unbounded memory as
// the server keeps marking entities dirty for it. When the cap is hit we
// clear the set and flip full_resync_needed; the next successful sync from
// that client triggers a full snapshot rather than a delta — which is what
// they would have needed anyway after that long a gap.
inline constexpr std::size_t kDirtyEntitiesHardCap = 65536;

// Server-side synchronization manager.
//
// Parameterized over the grlx-rpc channel type so callers can pick between
// plain tcp_channel and ssl_channel (with mTLS) at the app level. The
// `sync_server<Components...>` alias below preserves the historical
// "always TCP" contract; new apps that need TLS instantiate
// sync_server_with_channel directly with ssl_channel and forward an
// ssl::context into the constructor.
//
// Member function bodies live in sync_server_impl.hpp. Tiny one-liners
// remain inline here. Heavy bodies (anything using spdlog, cereal types,
// random/sstream/iomanip, or with non-trivial lambdas) are out-of-line so
// TUs that just need the type don't pay parse cost for them.
template <typename ChannelT, typename... SyncComponentsT>
class sync_server_with_channel {

  using channel_type = ChannelT;
  using rpc_server   = grlx::rpc::server<channel_type>;
  using tcp          = asio::ip::tcp;

public:
  template <typename... ChannelArgs>
  explicit sync_server_with_channel(entt_ext::ecs& ecs_instance, ChannelArgs&&... channel_args);

  asio::awaitable<void> start(uint16_t port) {
    auto endpoint = tcp::endpoint(asio::ip::address_v6::any(), port);
    co_await start(endpoint);
  }

  asio::awaitable<void> start(tcp::endpoint const& endpoint) {
    co_await rpc_server_.start(endpoint);
  }

  asio::awaitable<void> stop() {
    co_await rpc_server_.stop();
  }

  // Entity creation endpoint - clients request server entities
  asio::awaitable<entity_create_response> handle_entity_create(entity_create_request const& request);

  // Entity destruction endpoint - clients notify server about entity destruction
  asio::awaitable<entity_destroy_response> handle_entity_destroy(entity_destroy_request const& request);

  // Handshake endpoint for session creation
  asio::awaitable<handshake_response> handle_handshake_request(handshake_request const& request);

  // RPC endpoint for synchronization
  asio::awaitable<sync_response> handle_sync_request(sync_request const& request);

  // Lightweight session keepalive — touches last_sync for the given
  // session_id so cleanup_disconnected_clients() doesn't evict a connected
  // but otherwise idle client. See sync_keepalive_request in sync_common.hpp
  // for the broader rationale.
  asio::awaitable<sync_keepalive_response> handle_sync_keepalive(sync_keepalive_request const& request);

  // Component-specific insert/update handlers (type-safe, no registry needed)
  template <typename ComponentT>
  asio::awaitable<component_update_response<ComponentT>> handle_component_update(component_update_request<ComponentT> const& request);

  // Component-specific removal handlers (type-safe, no registry needed)
  template <typename ComponentT>
  asio::awaitable<component_remove_response<ComponentT>> handle_component_remove(component_remove_request<ComponentT> const& request);

  // Mark entity as dirty for all clients (will be synced if it has sync components)
  void mark_entity_for_sync(entity entt) {
    mark_entity_dirty_for_all_clients(entt);
  }

  // Create server entity
  entity create_server_entity() {
    return ecs_.create();
  }

  // Add component to server entity (will automatically notify clients via observers)
  template <typename ComponentT, typename... ArgsT>
  ComponentT& add_server_component(entity server_entity, ArgsT&&... args) {
    return ecs_.template emplace<ComponentT>(server_entity, std::forward<ArgsT>(args)...);
  }

  // Update component on server entity (will automatically notify clients via observers)
  template <typename ComponentT>
  ComponentT& update_server_component(entity server_entity, ComponentT const& component) {
    return ecs_.template emplace_or_replace<ComponentT>(server_entity, component);
  }

  // Remove component from server entity (will automatically notify clients via observers)
  template <typename ComponentT>
  size_t remove_server_component(entity server_entity) {
    return ecs_.template remove<ComponentT>(server_entity);
  }

  // Get client count for monitoring
  size_t get_client_count() const {
    return client_states_.size();
  }

  // Enable/disable real-time notifications (default: enabled)
  void set_notifications_enabled(bool enabled) {
    notifications_enabled_ = enabled;
  }

  bool are_notifications_enabled() const {
    return notifications_enabled_;
  }

  // Evict client states whose last_sync is older than client_idle_timeout_.
  //
  // The sync server hands out fresh session_ids on every handshake but has
  // no way to know when a client disconnects: each session_id lives in
  // client_states_ until removed explicitly. Without this sweep, every
  // reconnect (TLS-handshake / TCP-blip / keepalive miss) leaves an
  // orphaned entry whose dirty_entities set grows on every server-side
  // change — that's the >80 GB leak we observed in production. The sweep
  // is O(N_states) and called from session-close hooks plus periodically;
  // both paths are cheap.
  std::size_t cleanup_disconnected_clients();

  // Time after which a client_sync_state with no sync activity is considered
  // disconnected and may be evicted by cleanup_disconnected_clients(). Must
  // exceed the rpc-layer idle_timeout (default 5 min) plus client sync
  // cadence so legitimate clients aren't killed mid-flight; the default of
  // 15 minutes is comfortably above both.
  void set_client_idle_timeout(std::chrono::milliseconds timeout) {
    client_idle_timeout_ = timeout;
  }

  std::chrono::milliseconds get_client_idle_timeout() const noexcept {
    return client_idle_timeout_;
  }

  // Remove client and cleanup their entities
  void remove_client(std::string const& client_id);

private:
  // Set up automatic sync for a specific component type (server-side changes)
  template <typename ComponentT>
  void setup_automatic_sync(entt_ext::ecs& ecs);

  // Implementation of automatic sync setup for a single component.
  // ReadOnly=true for server_only components: skip apply-from-client observers.
  template <typename ComponentT, bool ReadOnly = false>
  void setup_automatic_sync_impl(entt_ext::ecs& ecs);

  // Send component update notification to all clients (server-initiated changes).
  //
  // rpc_server_.notify() already broadcasts to every active session, so we
  // emit the request exactly once per server-side change. Iterating
  // client_states_ here used to multiply each broadcast by the size of
  // client_states_ (which leaks one entry per reconnect because
  // handle_sync_request never cleans up), saturating each session's
  // 256-slot write_channel within milliseconds and getting the new client
  // killed by the channel-full path in rpc_server_.notify().
  template <typename ComponentT>
  asio::awaitable<void> notify_component_update_to_all_clients(entity server_entity, version_type sync_version, ComponentT const& component_data);

  template <typename ComponentT>
  asio::awaitable<void>
  notify_component_update_to_client(entity server_entity, version_type sync_version, ComponentT const& component, std::string const& client_id);

  template <typename ComponentT>
  asio::awaitable<void> notify_component_removal_to_client(entity server_entity, version_type sync_version, std::string const& client_id);

  // Send component removal notification to all clients (server-initiated changes).
  // See notify_component_update_to_all_clients above for why we don't iterate
  // client_states_ here.
  template <typename ComponentT>
  asio::awaitable<void> notify_component_removal_to_all_clients(entity server_entity, version_type sync_version);

  // Send component update notification to all other clients
  template <typename ComponentT>
  asio::awaitable<void> notify_component_update_to_other_clients(entity             server_entity,
                                                                 version_type       sync_version,
                                                                 ComponentT const&  component_data,
                                                                 std::string const& except_client_id);

  // Send component removal notification to all other clients
  template <typename ComponentT>
  asio::awaitable<void>
  notify_component_removal_to_other_clients(entity server_entity, version_type sync_version, std::string const& except_client_id);

  // ============================================================================
  // Entity Destruction Notification Functions
  // ============================================================================

  // Send entity destruction notification to a specific client
  asio::awaitable<void> notify_entity_destruction_to_client(entity server_entity, version_type sync_version, std::string const& client_id);

  // Send entity destruction notification to all clients (server-initiated changes).
  // See notify_component_update_to_all_clients above for why we don't iterate
  // client_states_ here.
  asio::awaitable<void> notify_entity_destruction_to_all_clients(entity server_entity, version_type sync_version);

  // Send entity destruction notification to all other clients (except the one that initiated it)
  asio::awaitable<void>
  notify_entity_destruction_to_other_clients(entity server_entity, version_type sync_version, std::string const& except_client_id);

  template <typename ComponentT>
  void copy_component_if_exists(entity src_entity, entity dst_entity, entt::registry& dst_registry) {
    if (auto* component = ecs_.template try_get<ComponentT>(src_entity)) {
      dst_registry.emplace<ComponentT>(dst_entity, *component);
    }
  }

  template <typename ComponentT>
  void copy_component_from_temp_if_exists(entity target_entity, entt::registry& temp_registry) {
    if (auto* component = temp_registry.try_get<ComponentT>(target_entity)) {
      ecs_.template emplace_or_replace<ComponentT>(target_entity, *component);
    }
  }

  template <typename ComponentT>
  void copy_component_from_temp_to_server(entity client_entity, entity server_entity, entt::registry& temp_registry) {
    if (auto* component = temp_registry.try_get<ComponentT>(client_entity)) {
      ecs_.template emplace_or_replace<ComponentT>(server_entity, *component);
    }
  }

  std::vector<entity> get_sync_enabled_entities() {
    std::vector<entity> entities;
    // Get all entities that have any of the sync components (including hierarchy)
    (collect_component_and_hierarchy_entities<SyncComponentsT>(entities), ...);

    // Remove duplicates
    std::sort(entities.begin(), entities.end());
    entities.erase(std::unique(entities.begin(), entities.end()), entities.end());

    return entities;
  }

  // Helper to save component and its hierarchy components to archive
  template <typename ComponentT>
  void save_component_and_hierarchy(cereal::PortableBinaryOutputArchive& archive);

  // Multi-tenant variant — kept for reference. Iterator-range
  // snapshot::get filters components but leaves the entity table
  // unfiltered (entt has no iterator overload for entity types). The
  // current snapshot path uses build_filtered_registry +
  // save_component_and_hierarchy_from instead, which filters both.
  template <typename ComponentT>
  void save_component_and_hierarchy_filtered(cereal::PortableBinaryOutputArchive& archive,
                                             std::vector<entity> const&           visible);

  // Save component (and hierarchy parent<T>/children<T>) from the given
  // registry. Used by the multi-tenant snapshot path so the entity
  // table itself is filtered and no other-tenant ids leak onto the
  // wire — the source registry here is a tmp built by
  // build_filtered_registry.
  template <typename ComponentT>
  void save_component_and_hierarchy_from(entt::registry&                      reg,
                                         cereal::PortableBinaryOutputArchive& archive);

  // Populate a temporary registry with the visible entities (preserving
  // their actual server IDs via create(hint)) and copy each sync
  // component plus its hierarchy components. Children<T> sets and
  // parent<T> refs that point at not-visible entities are dropped to
  // keep the snapshot self-consistent and prevent cross-tenant id
  // leaks via hierarchy fields.
  template <typename... ComponentsT>
  void build_filtered_registry(entt::registry& tmp, std::vector<entity> const& visible) {
    std::unordered_set<entity> visible_set(visible.begin(), visible.end());

    for (auto e : visible) {
      // entt::create(hint) honors the hint exactly when the slot is
      // free, which it always is on a fresh registry. The returned
      // entity matches the source server id.
      [[maybe_unused]] auto created = tmp.create(e);
    }

    auto copy_one = [&]<typename T>() {
      using ActualT = unwrap_hierarchy_t<T>;
      for (auto e : visible) {
        if (auto* c = ecs_.template try_get<ActualT>(e)) {
          tmp.template emplace<ActualT>(e, *c);
        }
      }
      if constexpr (is_with_hierarchy_v<T>) {
        for (auto e : visible) {
          if (auto* p = ecs_.template try_get<entt_ext::parent<ActualT>>(e)) {
            if (visible_set.contains(p->entity)) {
              tmp.template emplace<entt_ext::parent<ActualT>>(e, *p);
            }
          }
          if (auto* c = ecs_.template try_get<entt_ext::children<ActualT>>(e)) {
            entt_ext::children<ActualT> filtered;
            for (auto child : *c) {
              if (visible_set.contains(child)) {
                filtered.insert(child);
              }
            }
            // Emplace even when empty: the component's PRESENCE is state
            // (an empty group/hierarchy node must still reach the client, or
            // views over children<T> lose the entity after every reconnect).
            // Only the filtered-out member ids were the tenant-leak concern.
            tmp.template emplace<entt_ext::children<ActualT>>(e, std::move(filtered));
          }
        }
      }
    };

    (copy_one.template operator()<ComponentsT>(), ...);
  }

  // Look up the (user_id, role) for a given session_id. Returns nullptr
  // if the session_id isn't in client_states_ (e.g. expired session, or
  // an internal call that didn't go through the handshake).
  client_sync_state const* lookup_session_identity(std::string const& session_id) const {
    auto it = client_states_.find(session_id);
    if (it == client_states_.end()) return nullptr;
    return &it->second;
  }

  // Refresh `last_sync` for an existing session. No-op if the session
  // isn't in client_states_. Call this at the entry of every authenticated
  // handler so cleanup_disconnected_clients() can't evict an active
  // client that happens to be sending writes only (component updates,
  // removes, entity destroys) — those paths previously consulted
  // lookup_session_identity (const) and therefore never touched
  // last_sync, which silently expired the session after
  // client_idle_timeout_ even while the client was still talking.
  void touch_session(std::string const& session_id) {
    if (auto it = client_states_.find(session_id); it != client_states_.end()) {
      it->second.last_sync = std::chrono::steady_clock::now();
    }
  }

  // Single-entity multi-tenant visibility check. An entity is visible
  // to a session iff:
  //   - the session has admin role (1), or
  //   - the entity has no `owner` component (unowned/global), or
  //   - the entity's owner.user_id matches the session's user_id.
  // Used by both the snapshot path and the notification paths.
  bool is_entity_visible_to(entity e, std::string const& user_id, int role) {
    if (role == 1) return true;
    auto& reg           = ecs_.registry();
    auto& owner_storage = reg.template storage<owner>();
    if (!owner_storage.contains(e)) return true;
    return owner_storage.get(e).user_id == user_id;
  }

  // Build the list of entities visible to the requesting session.
  std::vector<entity> collect_visible_entities(std::string const& user_id, int role) {
    std::vector<entity> out;

    auto& reg            = ecs_.registry();
    auto& entity_storage = reg.template storage<entity>();
    for (auto e : entity_storage) {
      if (is_entity_visible_to(e, user_id, role)) out.push_back(e);
    }

    return out;
  }

  template <typename ComponentT>
  void collect_component_and_hierarchy_entities(std::vector<entity>& entities) {
    using ActualT = unwrap_hierarchy_t<ComponentT>;

    // Collect entities with the component itself
    collect_entities_with_component<ActualT>(entities);

    // Also collect entities with hierarchy components if wrapped with with_hierarchy<T>
    if constexpr (is_with_hierarchy_v<ComponentT>) {
      collect_entities_with_component<entt_ext::parent<ActualT>>(entities);
      collect_entities_with_component<entt_ext::children<ActualT>>(entities);
    }
  }

  template <typename ComponentT>
  void collect_entities_with_component(std::vector<entity>& entities) {
    for (auto [entt, _] : ecs_.template view<ComponentT>().each()) {
      entities.push_back(entt);
    }
  }

  // Check if entity has any of the sync components
  bool has_sync_components(entity entt) const {
    return (has_component_or_hierarchy<SyncComponentsT>(entt) || ...);
  }

  // Check if entity has a specific component or its hierarchy components
  template <typename ComponentT>
  bool has_component_or_hierarchy(entity entt) const {
    using ActualT = unwrap_hierarchy_t<ComponentT>;

    // Check the component itself
    bool has_comp = ecs_.template any_of<ActualT>(entt);

    // Also check hierarchy components if wrapped with with_hierarchy<T>
    if constexpr (is_with_hierarchy_v<ComponentT>) {
      has_comp = has_comp || ecs_.template any_of<entt_ext::parent<ActualT>>(entt) || ecs_.template any_of<entt_ext::children<ActualT>>(entt);
    }

    return has_comp;
  }

  void setup_rpc_endpoints(entt_ext::ecs& ecs);

  template <typename ComponentT>
  void register_component_endpoints(entt_ext::ecs& ecs);

  template <typename ComponentT, bool ReadOnly = false>
  void register_component_endpoints_impl(entt_ext::ecs& ecs);

  // Client state management. Touches last_sync on every access so the
  // stale-state sweep in cleanup_disconnected_clients() reflects activity,
  // not just initial-create time.
  client_sync_state& get_or_create_client_state(std::string const& client_id) {
    auto const now = std::chrono::steady_clock::now();
    auto it = client_states_.find(client_id);
    if (it == client_states_.end()) {
      auto [inserted_it, success]   = client_states_.emplace(client_id, client_sync_state{});
      inserted_it->second.client_id = client_id;
      inserted_it->second.last_sync = now;
      return inserted_it->second;
    }
    it->second.last_sync = now;
    return it->second;
  }

  // Insert into a per-client dirty set, enforcing the hard cap. When the
  // cap is hit we drop the set and mark the client for full resync — see
  // kDirtyEntitiesHardCap. This caps the per-client memory contribution
  // even if the client has gone silent without disconnecting cleanly.
  static void mark_dirty_capped(client_sync_state& state, entity entt);

  // Mark entity as dirty for all clients except the specified one
  void mark_entity_dirty_for_all_other_clients(entity entt, std::string const& except_client_id) {
    for (auto& [client_id, client_state] : client_states_) {
      if (client_id != except_client_id) {
        mark_dirty_capped(client_state, entt);
      }
    }
  }

  // Mark entity as dirty for all clients
  void mark_entity_dirty_for_all_clients(entity entt) {
    for (auto& [client_id, client_state] : client_states_) {
      mark_dirty_capped(client_state, entt);
    }
  }

  // Validate session ID (helper method)
  bool is_valid_session(std::string const& session_id) const {
    return !session_id.empty() && client_states_.find(session_id) != client_states_.end();
  }

  // Generate unique session ID
  std::string generate_session_id();

public:
  // Access the underlying RPC server (e.g. to send custom notifications)
  rpc_server& get_rpc_server() { return rpc_server_; }

  // Set an authentication handler called during handshake.
  // The handler receives the request and returns a handshake_response.
  // If the handler returns success=true, the server proceeds with session creation.
  // If the handler returns success=false, the handshake is rejected.
  using auth_handler_type = std::function<handshake_response(handshake_request const&)>;
  void set_auth_handler(auth_handler_type handler) { auth_handler_ = std::move(handler); }

  // WI-4 (fail-closed handshake): when NO auth handler is installed the
  // handshake is rejected by default. An app that deliberately runs without
  // authentication — e.g. a 127.0.0.1-only loopback control server — must
  // consciously opt in here, which grants the single local peer an admin
  // session. NEVER enable this on a listener reachable from an untrusted
  // network: it turns the server into an open door.
  void set_allow_anonymous(bool v) { allow_anonymous_ = v; }
  bool get_allow_anonymous() const noexcept { return allow_anonymous_; }

private:
  // Declaration order matches the constructor initializer list; C++ initializes
  // members in declaration order regardless of the list, so these must agree.
  ecs&                                               ecs_;
  std::string                                        protocol_version_;                // Protocol version based on component types
  rpc_server                                         rpc_server_;
  std::unordered_map<std::string, client_sync_state> client_states_;                   // Per-client sync state
  bool                                               notifications_enabled_   = true;  // Enable real-time notifications by default
  bool                                               applying_client_changes_ = false; // Flag to prevent sync loops
  auth_handler_type                                  auth_handler_;                    // Optional authentication handler
  std::chrono::milliseconds                          client_idle_timeout_     = std::chrono::minutes(15); // Stale-state eviction threshold
  bool                                               allow_anonymous_         = false;  // WI-4: opt-in anonymous handshake (loopback/trusted only)
};

// Backward-compatible alias: historical `sync_server<Components...>` usage
// keeps working and defaults to plain tcp_channel. Apps that want SSL /
// mTLS instantiate sync_server_with_channel<ssl_channel<...>, Components...>
// directly (typically via sync_list_traits::apply_with_channel).
template <typename... SyncComponentsT>
using sync_server = sync_server_with_channel<
    grlx::rpc::tcp_channel<grlx::rpc::binary_encoder>,
    SyncComponentsT...>;

} // namespace entt_ext::sync

// Auto-include the implementation by default. Apps that want to opt into
// the lean header (e.g. to centralize codegen via explicit instantiation)
// can define ENTT_EXT_SYNC_SERVER_NO_IMPL in their build system and pull
// in sync_server_impl.hpp only from a single per-app instantiation TU.
#if !defined(ENTT_EXT_SYNC_SERVER_NO_IMPL)
#  include <entt_ext/sync_server_impl.hpp>
#endif
