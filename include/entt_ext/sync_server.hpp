#pragma once

#include "ecs.hpp"
#include "entity_mapping.hpp"
#include "sync_common.hpp"
#include "type_name.hpp"

#include <grlx/rpc/encoder.hpp>
#include <grlx/rpc/message.hpp>
#include <grlx/rpc/server.hpp>
#include <grlx/rpc/tcp_channel.hpp>

#include <cereal/archives/portable_binary.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/unordered_map.hpp>
#include <cereal/types/unordered_set.hpp>
#include <cereal/types/vector.hpp>

#include <boost/asio/awaitable.hpp>

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <random>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <spdlog/spdlog.h>

namespace entt_ext::sync {

namespace asio = boost::asio;

// Per-client sync state
struct client_sync_state {
  std::chrono::steady_clock::time_point last_sync;
  std::chrono::steady_clock::time_point last_push;
  std::unordered_set<entity>            dirty_entities; // Entities that changed since last sync to this client
  std::string                           client_id;
  entity_mapping                        mapping; // Entity mapping for this client

  template <typename Archive>
  void serialize(Archive& archive) {
    archive(last_sync, last_push, dirty_entities, client_id, mapping);
  }
};

// Server-side synchronization manager
template <typename... SyncComponentsT>
class sync_server {

  using channel_type = grlx::rpc::tcp_channel<grlx::rpc::binary_encoder>;
  using rpc_server   = grlx::rpc::server<channel_type>;
  using tcp          = asio::ip::tcp;

public:
  explicit sync_server(entt_ext::ecs& ecs_instance)
    : ecs_(ecs_instance)
    , protocol_version_(sync_component_list<SyncComponentsT...>::generate_protocol_version()) {
    // Set up RPC endpoints
    setup_rpc_endpoints(ecs_instance);

    // Set up automatic synchronization for each component type
    (setup_automatic_sync<SyncComponentsT>(ecs_instance), ...);

    spdlog::info("Sync server initialized with protocol version: {}", protocol_version_);
  }

  auto start(uint16_t port) -> asio::awaitable<void> {

    auto endpoint = tcp::endpoint(asio::ip::address_v6::any(), port);
    co_await start(endpoint);
  }

  auto start(tcp::endpoint const& endpoint) -> asio::awaitable<void> {
    co_await rpc_server_.start(endpoint);
  }

  // Stop the server
  asio::awaitable<void> stop() {
    co_await rpc_server_.stop();
  }

  // Handshake endpoint for session creation
  asio::awaitable<handshake_response> handle_handshake_request(handshake_request const& request) {
    try {
      // Validate protocol version
      if (!request.protocol_version.empty() && request.protocol_version != protocol_version_) {
        std::string error_msg = "Protocol version mismatch! Server: " + protocol_version_ + ", Client: " + request.protocol_version;
        spdlog::error("{}", error_msg);
        co_return handshake_response{.success          = false,
                                     .session_id       = "",
                                     .error_message    = error_msg,
                                     .protocol_version = protocol_version_,
                                     .server_timestamp = std::chrono::steady_clock::now()};
      }

      // Generate unique session ID
      std::string session_id = generate_session_id();

      // Create client state for this session
      get_or_create_client_state(session_id);

      spdlog::info("Client connected - Name: {}, Version: {}, Session: {}",
                   request.client_name.empty() ? "unknown" : request.client_name,
                   request.client_version.empty() ? "unknown" : request.client_version,
                   session_id);

      co_return handshake_response{.success          = true,
                                   .session_id       = session_id,
                                   .error_message    = "",
                                   .protocol_version = protocol_version_,
                                   .server_timestamp = std::chrono::steady_clock::now()};

    } catch (std::exception const& ex) {
      co_return handshake_response{.success          = false,
                                   .session_id       = "",
                                   .error_message    = std::string("Handshake failed: ") + ex.what(),
                                   .protocol_version = protocol_version_,
                                   .server_timestamp = std::chrono::steady_clock::now()};
    }
  }

  // RPC endpoint for synchronization
  asio::awaitable<sync_response> handle_sync_request(sync_request const& request) {
    // Use session ID from request
    std::string const& client_id = request.session_id;
    if (client_id.empty()) {
      // Return empty response for invalid session
      co_return sync_response{.server_timestamp = std::chrono::steady_clock::now(), .snapshot_data = {}};
    }

    auto& client_state = get_or_create_client_state(client_id);

    // Create snapshot of requested entities with sync-enabled components
    grlx::rpc::buffer_type snapshot_buffer;

    try {
      grlx::rpc::ovectorstream            ostream;
      cereal::PortableBinaryOutputArchive archive(ostream);

      // Serialize the temporary registry
      entt::snapshot{ecs_.registry()}.get<entity>(archive);
      (entt::snapshot{ecs_.registry()}.template get<SyncComponentsT>(archive), ...);

      ostream.swap_vector(snapshot_buffer);

      // Update client's sync state
      client_state.last_sync = std::chrono::steady_clock::now();
      client_state.dirty_entities.clear();

    } catch (...) {
      // Return empty response on error
      snapshot_buffer.clear();
    }

    co_return sync_response{.server_timestamp = std::chrono::steady_clock::now(), .snapshot_data = std::move(snapshot_buffer)};
  }

  // Component-specific insert/update handlers (type-safe, no registry needed)
  template <typename ComponentT>
  asio::awaitable<component_update_response<ComponentT>> handle_component_update(component_update_request<ComponentT> const& request) {
    try {
      spdlog::debug("Handling component {} update: {} {}",
                    type_name<ComponentT>(),
                    static_cast<int>(request.target_entity),
                    std::chrono::duration_cast<std::chrono::milliseconds>(request.sync_version.time_since_epoch()).count());

      auto&  client_state  = get_or_create_client_state(request.session_id);
      entity client_entity = request.target_entity;

      // Check if we already have a server entity for this client entity
      entity server_entity;
      if (auto existing_server = client_state.mapping.get_server_entity(client_entity)) {
        server_entity = *existing_server;
        if (!ecs_.valid(server_entity)) {
          // Server entity was destroyed, create new one
          server_entity = ecs_.create();
          client_state.mapping.add_mapping(client_entity, server_entity);
        }
      } else {
        // Create new server entity using ECS
        server_entity = ecs_.create();
        client_state.mapping.add_mapping(client_entity, server_entity);
      }

      // if (auto sync_state = ecs_.template try_get<sync_component_state<ComponentT>>(server_entity); sync_state != nullptr) {
      //   if (sync_state->sync_version >= request.sync_version) {
      //     co_return component_update_response<ComponentT>{.success          = false,
      //                                                     .resulting_entity = client_entity,
      //                                                     .server_entity    = server_entity,
      //                                                     .error_message    = "Sync version is already up to date"};
      //   }
      // }

      ecs_.template emplace_or_replace<component_update_request<ComponentT>>(server_entity, request);

      spdlog::debug("Component {} update handled: {} {} {}",
                    type_name<ComponentT>(),
                    static_cast<int>(client_entity),
                    static_cast<int>(server_entity),
                    std::chrono::duration_cast<std::chrono::milliseconds>(request.sync_version.time_since_epoch()).count());

      co_return component_update_response<ComponentT>{.success          = true,
                                                      .resulting_entity = client_entity,
                                                      .server_entity    = server_entity,
                                                      .error_message    = ""};

    } catch (std::exception const& ex) {
      spdlog::error("Error handling component {} update:  {}", type_name<ComponentT>(), ex.what());
      co_return component_update_response<ComponentT>{.success          = false,
                                                      .resulting_entity = entt_ext::null,
                                                      .server_entity    = entt_ext::null,
                                                      .error_message    = std::string("Exception: ") + ex.what()};
    }
  }

  // Component-specific removal handlers (type-safe, no registry needed)
  template <typename ComponentT>
  asio::awaitable<component_remove_response<ComponentT>> handle_component_remove(component_remove_request<ComponentT> const& request) {
    try {
      auto&  client_state  = get_or_create_client_state(request.session_id);
      entity client_entity = request.target_entity;

      spdlog::debug("Handling component removal: {} {} {}", type_name<ComponentT>(), request.session_id, static_cast<int>(client_entity));
      // Map client entity to server entity
      auto server_entity_opt = client_state.mapping.get_server_entity(client_entity);
      if (!server_entity_opt) {
        co_return component_remove_response<ComponentT>{.success = false, .error_message = "Entity mapping not found"};
      }

      entity server_entity = *server_entity_opt;

      // Validate server entity exists
      if (!ecs_.valid(server_entity)) {
        co_return component_remove_response<ComponentT>{.success = false, .error_message = "Server entity does not exist"};
      }

      spdlog::debug("Removing component {} from server entity: {}->{}",
                    type_name<ComponentT>(),
                    static_cast<int>(server_entity),
                    static_cast<int>(client_entity));
      co_await ecs_.template remove_deferred<ComponentT>(server_entity);

      co_return component_remove_response<ComponentT>{.success = true, .error_message = ""};

    } catch (std::exception const& ex) {
      co_return component_remove_response<ComponentT>{.success = false, .error_message = std::string("Exception: ") + ex.what()};
    }
  }
  // Mark entity as dirty for all clients (will be synced if it has sync components)
  void mark_entity_for_sync(entity entt) {
    // Mark as dirty for all clients
    mark_entity_dirty_for_all_clients(entt);
  }

  // Create server entity and ensure it gets mapped for all clients
  entity create_server_entity() {
    entity server_entity = ecs_.create();

    // Create client entity mappings for all connected clients
    for (auto& [client_id, client_state] : client_states_) {
      // Use server entity ID as client entity ID for simplicity
      // (clients will handle any ID conflicts during merge)
      client_state.mapping.add_mapping(server_entity, server_entity);
    }

    return server_entity;
  }

  // Add component to server entity (will automatically notify clients via observers)
  template <typename ComponentT, typename... ArgsT>
  ComponentT& add_server_component(entity server_entity, ArgsT&&... args) {
    // This will trigger the component observer and automatically notify clients
    return ecs_.template emplace<ComponentT>(server_entity, std::forward<ArgsT>(args)...);
  }

  // Update component on server entity (will automatically notify clients via observers)
  template <typename ComponentT>
  ComponentT& update_server_component(entity server_entity, ComponentT const& component) {
    // This will trigger the component observer and automatically notify clients
    return ecs_.template emplace_or_replace<ComponentT>(server_entity, component);
  }

  // Remove component from server entity (will automatically notify clients via observers)
  template <typename ComponentT>
  size_t remove_server_component(entity server_entity) {
    // This will trigger the component observer and automatically notify clients
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

  // Clean up disconnected clients (call periodically)
  void cleanup_disconnected_clients() {
    // In a real implementation, you'd check which clients are still connected
    // and remove their state. This is framework-dependent.
  }

  // Remove client and cleanup their entities
  void remove_client(std::string const& client_id) {
    auto it = client_states_.find(client_id);
    if (it != client_states_.end()) {
      // Get all server entities owned by this client
      auto server_entities = it->second.mapping.get_server_entities();

      // Optionally destroy client entities when client disconnects
      for (auto server_entity : server_entities) {
        if (ecs_.valid(server_entity)) {
          // Option 1: Destroy the entity completely
          // ecs_.destroy(server_entity);

          // Option 2: Keep entity (it becomes server-managed)
          // Do nothing - entity remains in server ECS
        }
      }

      // Remove client state
      client_states_.erase(it);
    }
  }

  // Get entity mapping for a specific client (for debugging)
  entity_mapping const* get_client_mapping(std::string const& client_id) const {
    auto it = client_states_.find(client_id);
    return it != client_states_.end() ? &it->second.mapping : nullptr;
  }

private:
  // Set up automatic sync for a specific component type (server-side changes)
  template <typename ComponentT>
  void setup_automatic_sync(entt_ext::ecs& ecs) {
    // Set up component observer to track server-side changes
    auto& observer = ecs.component_observer<ComponentT>();

    // When a sync component is added by the server, notify all clients
    observer.on_construct([this](entt_ext::ecs& ecs, entt_ext::entity e, ComponentT& component) -> void {
      spdlog::debug("Server-side component added: {} {}", type_name<ComponentT>(), static_cast<int>(e));
      if (auto request = ecs.template try_get<component_update_request<ComponentT>>(e); request != nullptr) {
        return;
      }
      auto sync_version = std::chrono::steady_clock::now();
      ecs.template emplace_or_replace<local_component_changed<ComponentT>>(e, sync_version);
    });

    // When a sync component is updated by the server, notify all clients
    observer.on_update([this](entt_ext::ecs& ecs, entt_ext::entity e, ComponentT& component) -> void {
      spdlog::debug("Server-side component updated: {} {}", type_name<ComponentT>(), static_cast<int>(e));
      if (auto request = ecs.template try_get<component_update_request<ComponentT>>(e); request != nullptr) {
        return;
      }
      auto sync_version = std::chrono::steady_clock::now();
      ecs.template emplace_or_replace<local_component_changed<ComponentT>>(e, sync_version);
    });

    // When a sync component is removed by the server, notify all clients
    observer.on_destroy([this](entt_ext::ecs& ecs, entt_ext::entity e, ComponentT& component) -> void {
      spdlog::debug("Server-side component removed: {} {}", type_name<ComponentT>(), static_cast<int>(e));
      // Notify all clients about the component removal
      if (auto request = ecs.template try_get<component_remove_request<ComponentT>>(e); request != nullptr) {
        return;
      }
      auto sync_version = std::chrono::steady_clock::now();
      ecs.template emplace_or_replace<local_component_removed<ComponentT>>(e, sync_version);
    });

    // Set up system to process component changes
    ecs.system<local_component_changed<ComponentT>, ComponentT>(entt::exclude<component_update_request<ComponentT>>)
        .each(
            [this](entt_ext::ecs&                       ecs,
                   entt_ext::system&                    self,
                   double                               dt,
                   entt_ext::entity                     e,
                   local_component_changed<ComponentT>& sync_data,
                   ComponentT&                          component) -> asio::awaitable<void> {
              // Notify all clients about the component update
              co_await notify_component_update_to_all_clients<ComponentT>(e, sync_data.sync_version, component);
              co_await ecs.template remove_deferred<local_component_changed<ComponentT>>(e);
              co_return;
            },
            entt_ext::run_policy_t<entt_ext::run_policy::detached>{})
        .stage(entt_ext::stage::update);

    ecs.system<component_update_request<ComponentT>, ComponentT>(entt::exclude<local_component_changed<ComponentT>>)
        .each(
            [this](entt_ext::ecs&                        ecs,
                   entt_ext::system&                     self,
                   double                                dt,
                   entt_ext::entity                      e,
                   component_update_request<ComponentT>& request,
                   ComponentT&                           component) -> asio::awaitable<void> {
              // Notify all clients about the component update
              co_await ecs.template replace_deferred<ComponentT>(e, request.component_data);
              co_await notify_component_update_to_other_clients<ComponentT>(e, request.sync_version, component, request.session_id);
              co_await ecs.template remove_deferred<component_update_request<ComponentT>>(e);
              co_return;
            },
            entt_ext::run_policy_t<entt_ext::run_policy::detached>{})
        .stage(entt_ext::stage::update);

    ecs.system<component_update_request<ComponentT>>(entt::exclude<ComponentT>)
        .each(
            [this](entt_ext::ecs& ecs, entt_ext::system& self, double dt, entt_ext::entity e, component_update_request<ComponentT>& request)
                -> asio::awaitable<void> {
              // Notify all clients about the component update
              co_await ecs.template emplace_deferred<ComponentT>(e, request.component_data);

              co_await notify_component_update_to_other_clients<ComponentT>(e, request.sync_version, request.component_data, request.session_id);
              co_await ecs.template remove_deferred<component_update_request<ComponentT>>(e);
              co_return;
            },
            entt_ext::run_policy_t<entt_ext::run_policy::detached>{})
        .stage(entt_ext::stage::update);

    ecs.system<component_remove_request<ComponentT>>(entt::exclude<local_component_removed<ComponentT>>)
        .each(
            [this](entt_ext::ecs& ecs, entt_ext::system& self, double dt, entt_ext::entity e, component_remove_request<ComponentT>& request)
                -> asio::awaitable<void> {
              co_await ecs.template remove_deferred<ComponentT>(e);
              co_await notify_component_removal_to_other_clients<ComponentT>(e, request.sync_version, request.session_id);
              co_await ecs.template remove_deferred<component_remove_request<ComponentT>>(e);
              co_return;
            },
            entt_ext::run_policy_t<entt_ext::run_policy::detached>{})
        .stage(entt_ext::stage::update);

    ecs.system<local_component_removed<ComponentT>>(entt::exclude<component_remove_request<ComponentT>>)
        .each(
            [this](entt_ext::ecs& ecs, entt_ext::system& self, double dt, entt_ext::entity e, local_component_removed<ComponentT>& sync_data)
                -> asio::awaitable<void> {
              co_await notify_component_removal_to_all_clients<ComponentT>(e, sync_data.sync_version);
              co_await ecs.template remove_deferred<local_component_removed<ComponentT>>(e);
              co_return;
            },
            entt_ext::run_policy_t<entt_ext::run_policy::detached>{})
        .stage(entt_ext::stage::update);
  }

  // Send component update notification to all clients (server-initiated changes)
  template <typename ComponentT>
  asio::awaitable<void> notify_component_update_to_all_clients(entity server_entity, version_type sync_version, ComponentT const& component_data) {
    if (!notifications_enabled_) {
      co_return; // Skip notifications if disabled
    }

    std::string component_name    = std::string(type_name<ComponentT>());
    std::string notification_name = "component_updated_" + component_name;

    // Notify all clients
    for (auto& [client_id, client_state] : client_states_) {
      // Map server entity to client entity for this client
      if (auto client_entity_opt = client_state.mapping.get_client_entity(server_entity)) {
        entity client_entity = *client_entity_opt;

        // Send notification with client entity ID and component data
        try {
          co_await notify_component_update_to_client(client_entity, sync_version, component_data, client_id);
        } catch (...) {
          // Log error or handle notification failure
          // Continue with other clients
        }
      }
    }
  }

  template <typename ComponentT>
  asio::awaitable<void> notify_component_update_to_client(entity e, version_type sync_version, ComponentT& component, std::string const& client_id) {
    spdlog::debug("Notifying component update to client: {} {} {} {}",
                  type_name<ComponentT>(),
                  static_cast<int>(e),
                  client_id,
                  std::chrono::duration_cast<std::chrono::milliseconds>(sync_version.time_since_epoch()).count());
    std::string endpoint_name = "component_updated_" + std::string(type_name<ComponentT>());

    component_update_request<ComponentT> request{.session_id     = client_id,
                                                 .sync_version   = sync_version,
                                                 .target_entity  = e,
                                                 .component_data = component};

    co_await rpc_server_.notify(endpoint_name, std::move(request));

    co_return;
  }

  template <typename ComponentT>
  asio::awaitable<void> notify_component_removal_to_client(entity e, version_type sync_version, std::string const& client_id) {
    spdlog::debug("Notifying component removal to client: {} {} {} {}",
                  type_name<ComponentT>(),
                  static_cast<int>(e),
                  client_id,
                  std::chrono::duration_cast<std::chrono::milliseconds>(sync_version.time_since_epoch()).count());

    std::string component_name    = std::string(type_name<ComponentT>());
    std::string notification_name = "component_removed_" + component_name;

    component_remove_request<ComponentT> request{.session_id = client_id, .sync_version = sync_version, .target_entity = e};

    try {
      co_await rpc_server_.notify(notification_name, std::move(request));
    } catch (...) {
      // Log error or handle notification failure
    }
    co_return;
  }

  // Send component removal notification to all clients (server-initiated changes)
  template <typename ComponentT>
  asio::awaitable<void> notify_component_removal_to_all_clients(entity server_entity, version_type sync_version) {
    if (!notifications_enabled_) {
      co_return; // Skip notifications if disabled
    }

    std::string component_name    = std::string(type_name<ComponentT>());
    std::string notification_name = "component_removed_" + component_name;

    // Notify all clients
    for (auto& [client_id, client_state] : client_states_) {
      // Map server entity to client entity for this client
      if (auto client_entity_opt = client_state.mapping.get_client_entity(server_entity)) {
        entity client_entity = *client_entity_opt;

        // Send notification with client entity ID

        component_remove_request<ComponentT> request{.session_id = client_id, .sync_version = sync_version, .target_entity = client_entity};
        try {
          co_await rpc_server_.notify(notification_name, std::move(request));
        } catch (...) {
          // Log error or handle notification failure
          // Continue with other clients
        }
      }
    }
  }

  // Send component update notification to all other clients
  template <typename ComponentT>
  asio::awaitable<void> notify_component_update_to_other_clients(entity             server_entity,
                                                                 version_type       sync_version,
                                                                 ComponentT const&  component_data,
                                                                 std::string const& except_client_id) {
    if (!notifications_enabled_) {
      co_return; // Skip notifications if disabled
    }

    std::string component_name    = std::string(type_name<ComponentT>());
    std::string notification_name = "component_updated_" + component_name;

    // Notify each client (except the one that made the change)
    for (auto& [client_id, client_state] : client_states_) {
      if (client_id == except_client_id)
        continue;

      // Map server entity to client entity for this client
      if (auto client_entity_opt = client_state.mapping.get_client_entity(server_entity)) {
        entity client_entity = *client_entity_opt;

        // Send notification with client entity ID and component data
        component_update_request<ComponentT> request{.session_id     = client_id,
                                                     .sync_version   = sync_version,
                                                     .target_entity  = client_entity,
                                                     .component_data = component_data};

        try {
          co_await rpc_server_.notify(notification_name, std::move(request));
        } catch (...) {
          // Log error or handle notification failure
          // Continue with other clients
        }
      }
    }
  }

  // Send component removal notification to all other clients
  template <typename ComponentT>
  asio::awaitable<void>
  notify_component_removal_to_other_clients(entity server_entity, version_type sync_version, std::string const& except_client_id) {
    if (!notifications_enabled_) {
      co_return; // Skip notifications if disabled
    }

    std::string component_name    = std::string(type_name<ComponentT>());
    std::string notification_name = "component_removed_" + component_name;

    // Notify each client (except the one that made the change)
    for (auto& [client_id, client_state] : client_states_) {
      if (client_id == except_client_id)
        continue;

      // Map server entity to client entity for this client
      if (auto client_entity_opt = client_state.mapping.get_client_entity(server_entity)) {
        entity client_entity = *client_entity_opt;

        // Send notification with client entity ID
        component_remove_request<ComponentT> request{.session_id = client_id, .sync_version = sync_version, .target_entity = client_entity};
        try {
          co_await rpc_server_.notify(notification_name, std::move(request));
        } catch (...) {
          // Log error or handle notification failure
          // Continue with other clients
        }
      }
    }
  }

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
    // Get all entities that have any of the sync components
    (collect_entities_with_component<SyncComponentsT>(entities), ...);

    // Remove duplicates
    std::sort(entities.begin(), entities.end());
    entities.erase(std::unique(entities.begin(), entities.end()), entities.end());

    return entities;
  }

  template <typename ComponentT>
  void collect_entities_with_component(std::vector<entity>& entities) {
    for (auto [entt, _] : ecs_.template view<ComponentT>().each()) {
      entities.push_back(entt);
    }
  }

  // Check if entity has any of the sync components
  bool has_sync_components(entity entt) const {
    return (ecs_.template any_of<SyncComponentsT>(entt) || ...);
  }

  void setup_rpc_endpoints(entt_ext::ecs& ecs) {
    // Register handshake endpoint (no session context needed for handshake)
    rpc_server_.attach("handshake", [this](handshake_request const& request) -> asio::awaitable<handshake_response> {
      co_return co_await handle_handshake_request(request);
    });

    // Register general sync endpoint
    rpc_server_.attach("sync", [this](sync_request const& request) -> asio::awaitable<sync_response> {
      // Note: For now, sync requests don't require session context since they work with anonymous clients
      // In a more complete implementation, you'd extract session_id from request or connection context
      co_return co_await handle_sync_request(request);
    });

    // Register component-specific endpoints using fold expression
    (register_component_endpoints<SyncComponentsT>(ecs), ...);
  }

  template <typename ComponentT>
  void register_component_endpoints(entt_ext::ecs& ecs) {
    std::string component_name = std::string(type_name<ComponentT>());

    // Register update endpoint: "update_component_Position", "update_component_Velocity", etc.
    // This handles both insert and update operations
    std::string update_endpoint = "component_updated_" + component_name;
    rpc_server_.attach(update_endpoint,
                       [this](component_update_request<ComponentT> const& request) -> asio::awaitable<component_update_response<ComponentT>> {
                         co_return co_await handle_component_update<ComponentT>(request);
                       });

    // Register remove endpoint: "remove_component_Position", "remove_component_Velocity", etc.
    std::string remove_endpoint = "component_removed_" + component_name;
    rpc_server_.attach(remove_endpoint,
                       [this](component_remove_request<ComponentT> const& request) -> asio::awaitable<component_remove_response<ComponentT>> {
                         co_return co_await handle_component_remove<ComponentT>(request);
                       });
  }

private:
  // Client state management
  client_sync_state& get_or_create_client_state(std::string const& client_id) {
    auto it = client_states_.find(client_id);
    if (it == client_states_.end()) {
      auto [inserted_it, success]   = client_states_.emplace(client_id, client_sync_state{});
      inserted_it->second.client_id = client_id;
      inserted_it->second.last_sync = std::chrono::steady_clock::now();
      return inserted_it->second;
    }
    return it->second;
  }

  // Mark entity as dirty for all clients except the specified one
  void mark_entity_dirty_for_all_other_clients(entity entt, std::string const& except_client_id) {
    for (auto& [client_id, client_state] : client_states_) {
      if (client_id != except_client_id) {
        client_state.dirty_entities.insert(entt);
      }
    }
  }

  // Mark entity as dirty for all clients
  void mark_entity_dirty_for_all_clients(entity entt) {
    for (auto& [client_id, client_state] : client_states_) {
      client_state.dirty_entities.insert(entt);
    }
  }

  // Validate session ID (helper method)
  bool is_valid_session(std::string const& session_id) const {
    return !session_id.empty() && client_states_.find(session_id) != client_states_.end();
  }

  // Generate unique session ID
  std::string generate_session_id() {
    static std::random_device                      rd;
    static std::mt19937                            gen(rd());
    static std::uniform_int_distribution<uint64_t> dis;

    auto timestamp   = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    auto random_part = dis(gen);

    std::stringstream ss;
    ss << "session_" << std::hex << timestamp << "_" << std::hex << random_part;
    return ss.str();
  }

private:
  ecs&                                               ecs_;
  rpc_server                                         rpc_server_;
  std::unordered_map<std::string, client_sync_state> client_states_;                   // Per-client sync state
  bool                                               notifications_enabled_   = true;  // Enable real-time notifications by default
  bool                                               applying_client_changes_ = false; // Flag to prevent sync loops
  std::string                                        protocol_version_;                // Protocol version based on component types
};

} // namespace entt_ext::sync