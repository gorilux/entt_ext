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

  template <typename Archive>
  void serialize(Archive& archive) {
    archive(last_sync, last_push, dirty_entities, client_id);
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

  // Entity creation endpoint - clients request server entities
  asio::awaitable<entity_create_response> handle_entity_create(entity_create_request const& request) {
    try {
      // Create new server entity
      entity server_entity = ecs_.create();

      spdlog::info("Created server entity {} for client entity {} (session {})",
                   static_cast<int>(server_entity),
                   static_cast<int>(request.client_entity),
                   request.session_id);

      co_return entity_create_response{.success = true, .server_entity = server_entity, .error_message = ""};

    } catch (std::exception const& ex) {
      spdlog::error("Error creating entity: {}", ex.what());
      co_return entity_create_response{.success = false, .server_entity = entt_ext::null, .error_message = std::string("Exception: ") + ex.what()};
    }
  }

  // Entity destruction endpoint - clients notify server about entity destruction
  asio::awaitable<entity_destroy_response> handle_entity_destroy(entity_destroy_request const& request) {
    try {
      entity server_entity = request.server_entity;

      spdlog::info("Destroying server entity {} (session {}, version {})",
                   static_cast<int>(server_entity),
                   request.session_id,
                   std::chrono::duration_cast<std::chrono::milliseconds>(request.sync_version.time_since_epoch()).count());

      // Validate server entity exists
      if (!ecs_.valid(server_entity)) {
        std::string error_msg = "Server entity does not exist";
        spdlog::warn("{}: {}", error_msg, static_cast<int>(server_entity));
        co_return entity_destroy_response{.success = false, .error_message = error_msg};
      }

      // Notify other clients about the entity destruction before destroying it
      try {
        co_await notify_entity_destruction_to_other_clients(server_entity, request.sync_version, request.session_id);
      } catch (std::exception const& ex) {
        spdlog::error("Error notifying entity destruction to other clients: {}", ex.what());
      } catch (...) {
        spdlog::error("Error notifying entity destruction to other clients: unknown exception");
      }

      // Destroy the server entity
      co_await ecs_.destroy_deferred(server_entity);

      spdlog::debug("Server entity {} destroyed successfully", static_cast<int>(server_entity));

      co_return entity_destroy_response{.success = true, .error_message = ""};

    } catch (std::exception const& ex) {
      spdlog::error("Error destroying entity: {}", ex.what());
      co_return entity_destroy_response{.success = false, .error_message = std::string("Exception: ") + ex.what()};
    }
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
      (save_component_and_hierarchy<SyncComponentsT>(archive), ...);

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

      // target_entity is always the server entity
      entity server_entity = request.target_entity;

      // Validate server entity exists
      if (!ecs_.valid(server_entity)) {
        co_return component_update_response<ComponentT>{.success = false, .error_message = "Server entity does not exist"};
      }

      co_await ecs_.template emplace_or_replace_deferred<component_update_request<ComponentT>>(server_entity, request);

      spdlog::debug("Component {} update handled: {} {}",
                    type_name<ComponentT>(),
                    static_cast<int>(server_entity),
                    std::chrono::duration_cast<std::chrono::milliseconds>(request.sync_version.time_since_epoch()).count());

      co_return component_update_response<ComponentT>{.success = true, .error_message = ""};

    } catch (std::exception const& ex) {
      spdlog::error("Error handling component {} update:  {}", type_name<ComponentT>(), ex.what());
      co_return component_update_response<ComponentT>{.success = false, .error_message = std::string("Exception: ") + ex.what()};
    }
  }

  // Component-specific removal handlers (type-safe, no registry needed)
  template <typename ComponentT>
  asio::awaitable<component_remove_response<ComponentT>> handle_component_remove(component_remove_request<ComponentT> const& request) {
    try {
      // target_entity is always the server entity
      entity server_entity = request.target_entity;

      spdlog::debug("Handling component removal: {} {} {}", type_name<ComponentT>(), request.session_id, static_cast<int>(server_entity));

      // Validate server entity exists
      if (!ecs_.valid(server_entity)) {
        co_return component_remove_response<ComponentT>{.success = false, .error_message = "Server entity does not exist"};
      }

      spdlog::debug("Removing component {} from server entity: {}", type_name<ComponentT>(), static_cast<int>(server_entity));
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

  // Create server entity
  entity create_server_entity() {
    entity server_entity = ecs_.create();
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
      // Remove client state
      client_states_.erase(it);
      spdlog::info("Removed client: {}", client_id);
    }
  }

private:
  // Set up automatic sync for a specific component type (server-side changes)
  template <typename ComponentT>
  void setup_automatic_sync(entt_ext::ecs& ecs) {
    // Set up for the component itself
    setup_automatic_sync_impl<ComponentT>(ecs);

    // // Also set up for hierarchy components if not already a hierarchy component
    // if constexpr (!is_hierarchy_component<ComponentT>::value) {
    //   setup_automatic_sync_impl<entt_ext::parent<ComponentT>>(ecs);
    //   setup_automatic_sync_impl<entt_ext::children<ComponentT>>(ecs);
    // }
  }

  // Implementation of automatic sync setup for a single component
  template <typename ComponentT>
  void setup_automatic_sync_impl(entt_ext::ecs& ecs) {
    // Set up component observer to track server-side changes
    auto& observer = ecs.component_observer<ComponentT>();

    // When a sync component is added by the server, notify all clients immediately
    observer.on_construct([this](entt_ext::ecs& ecs, entt_ext::entity e, ComponentT& component) -> asio::awaitable<void> {
      spdlog::debug("Server-side component added: {} {}", type_name<ComponentT>(), static_cast<int>(e));
      if (auto request = ecs.template try_get<component_update_request<ComponentT>>(e); request != nullptr) {
        co_return;
      }

      auto sync_version = std::chrono::steady_clock::now();

      try {
        co_await notify_component_update_to_all_clients<ComponentT>(e, sync_version, component);
      } catch (std::exception const& ex) {
        spdlog::error("Error notifying component update to all clients: {}", ex.what());
      } catch (...) {
        spdlog::error("Error notifying component update to all clients: unknown exception");
      }

      co_return;
    });

    // When a sync component is updated by the server, notify all clients immediately
    observer.on_update([this](entt_ext::ecs& ecs, entt_ext::entity e, ComponentT& component) -> asio::awaitable<void> {
      spdlog::debug("Server-side component updated: {} {}", type_name<ComponentT>(), static_cast<int>(e));
      if (auto request = ecs.template try_get<component_update_request<ComponentT>>(e); request != nullptr) {
        co_return;
      }

      auto sync_version = std::chrono::steady_clock::now();

      try {
        co_await notify_component_update_to_all_clients<ComponentT>(e, sync_version, component);
      } catch (std::exception const& ex) {
        spdlog::error("Error notifying component update to all clients: {}", ex.what());
      } catch (...) {
        spdlog::error("Error notifying component update to all clients: unknown exception");
      }

      co_return;
    });

    // When a sync component is removed by the server, notify all clients immediately
    observer.on_destroy([this](entt_ext::ecs& ecs, entt_ext::entity e, ComponentT& component) -> asio::awaitable<void> {
      spdlog::debug("Server-side component removed: {} {}", type_name<ComponentT>(), static_cast<int>(e));
      if (auto request = ecs.template try_get<component_remove_request<ComponentT>>(e); request != nullptr) {
        co_return;
      }

      auto sync_version = std::chrono::steady_clock::now();

      try {
        co_await notify_component_removal_to_all_clients<ComponentT>(e, sync_version);
      } catch (std::exception const& ex) {
        spdlog::error("Error notifying component removal to all clients: {}", ex.what());
      } catch (...) {
        spdlog::error("Error notifying component removal to all clients: unknown exception");
      }

      co_return;
    });

    // Set up observer for component_update_request to apply updates from clients
    auto& update_request_observer = ecs.component_observer<component_update_request<ComponentT>>();

    update_request_observer.on_construct(
        [this](entt_ext::ecs& ecs, entt_ext::entity e, component_update_request<ComponentT>& request) -> asio::awaitable<void> {
          try {
            // Apply the component update
            spdlog::debug("Applying component update request: {} server={} client={} version={}",
                          type_name<ComponentT>(),
                          static_cast<int>(e),
                          std::chrono::duration_cast<std::chrono::milliseconds>(request.sync_version.time_since_epoch()).count(),
                          request.session_id);
            ecs.template emplace_or_replace<ComponentT>(e, request.component_data);

            // Notify all other clients about the component update
            co_await notify_component_update_to_other_clients<ComponentT>(e, request.sync_version, request.component_data, request.session_id);

            // Remove the request marker
            co_await ecs.template remove_deferred<component_update_request<ComponentT>>(e);
          } catch (std::exception const& ex) {
            spdlog::error("Error applying component update request: {}", ex.what());
          } catch (...) {
            spdlog::error("Error applying component update request: unknown exception");
          }

          co_return;
        });

    // Set up observer for component_remove_request to apply removals from clients
    auto& remove_request_observer = ecs.component_observer<component_remove_request<ComponentT>>();

    remove_request_observer.on_construct(
        [this](entt_ext::ecs& ecs, entt_ext::entity e, component_remove_request<ComponentT>& request) -> asio::awaitable<void> {
          try {
            ecs.template remove<ComponentT>(e);
            co_await notify_component_removal_to_other_clients<ComponentT>(e, request.sync_version, request.session_id);
            co_await ecs.template remove_deferred<component_remove_request<ComponentT>>(e);
          } catch (std::exception const& ex) {
            spdlog::error("Error applying component remove request: {}", ex.what());
          } catch (...) {
            spdlog::error("Error applying component remove request: unknown exception");
          }

          co_return;
        });
  }

  // Send component update notification to all clients (server-initiated changes)
  template <typename ComponentT>
  asio::awaitable<void> notify_component_update_to_all_clients(entity server_entity, version_type sync_version, ComponentT const& component_data) {
    if (!notifications_enabled_) {
      co_return; // Skip notifications if disabled
    }

    std::string component_name    = std::string(type_name<ComponentT>());
    std::string notification_name = "component_updated_" + component_name;

    // Notify all clients with server entity ID
    for (auto& [client_id, client_state] : client_states_) {
      try {
        co_await notify_component_update_to_client(server_entity, sync_version, component_data, client_id);
      } catch (...) {
        // Log error or handle notification failure
        // Continue with other clients
      }
    }
  }

  template <typename ComponentT>
  asio::awaitable<void>
  notify_component_update_to_client(entity server_entity, version_type sync_version, ComponentT const& component, std::string const& client_id) {
    spdlog::debug("Notifying component update to client: {} server_entity={} client={} {}",
                  type_name<ComponentT>(),
                  static_cast<int>(server_entity),
                  client_id,
                  std::chrono::duration_cast<std::chrono::milliseconds>(sync_version.time_since_epoch()).count());
    std::string endpoint_name = "component_updated_" + std::string(type_name<ComponentT>());

    // Send server entity in target_entity field
    component_update_request<ComponentT> request{.session_id     = client_id,
                                                 .sync_version   = sync_version,
                                                 .target_entity  = server_entity,
                                                 .component_data = component};

    co_await rpc_server_.notify(endpoint_name, std::move(request));

    co_return;
  }

  template <typename ComponentT>
  asio::awaitable<void> notify_component_removal_to_client(entity server_entity, version_type sync_version, std::string const& client_id) {
    spdlog::debug("Notifying component removal to client: {} server_entity={} client={} {}",
                  type_name<ComponentT>(),
                  static_cast<int>(server_entity),
                  client_id,
                  std::chrono::duration_cast<std::chrono::milliseconds>(sync_version.time_since_epoch()).count());

    std::string component_name    = std::string(type_name<ComponentT>());
    std::string notification_name = "component_removed_" + component_name;

    // Send server entity in target_entity field
    component_remove_request<ComponentT> request{.session_id = client_id, .sync_version = sync_version, .target_entity = server_entity};

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

    // Notify all clients with server entity ID
    for (auto& [client_id, client_state] : client_states_) {
      component_remove_request<ComponentT> request{.session_id = client_id, .sync_version = sync_version, .target_entity = server_entity};
      try {
        co_await rpc_server_.notify(notification_name, std::move(request));
      } catch (...) {
        // Log error or handle notification failure
        // Continue with other clients
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

    // Notify each client (except the one that made the change) with server entity ID
    for (auto& [client_id, client_state] : client_states_) {
      if (client_id == except_client_id)
        continue;

      spdlog::debug("Notifying component update to other client: {} server={} client={} except={} version={}",
                    type_name<ComponentT>(),
                    static_cast<int>(server_entity),
                    client_id,
                    except_client_id,
                    std::chrono::duration_cast<std::chrono::milliseconds>(sync_version.time_since_epoch()).count());
      // Send notification with server entity ID and component data
      component_update_request<ComponentT> request{.session_id     = client_id,
                                                   .sync_version   = sync_version,
                                                   .target_entity  = server_entity,
                                                   .component_data = component_data};

      try {
        co_await rpc_server_.notify(notification_name, std::move(request));
      } catch (...) {
        // Log error or handle notification failure
        // Continue with other clients
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

    // Notify each client (except the one that made the change) with server entity ID
    for (auto& [client_id, client_state] : client_states_) {
      if (client_id == except_client_id)
        continue;

      // Send notification with server entity ID
      component_remove_request<ComponentT> request{.session_id = client_id, .sync_version = sync_version, .target_entity = server_entity};
      try {
        co_await rpc_server_.notify(notification_name, std::move(request));
      } catch (...) {
        // Log error or handle notification failure
        // Continue with other clients
      }
    }
  }

  // ============================================================================
  // Entity Destruction Notification Functions
  // ============================================================================

  // Send entity destruction notification to a specific client
  asio::awaitable<void> notify_entity_destruction_to_client(entity server_entity, version_type sync_version, std::string const& client_id) {
    spdlog::debug("Notifying entity destruction to client: server_entity={} client={} version={}",
                  static_cast<int>(server_entity),
                  client_id,
                  std::chrono::duration_cast<std::chrono::milliseconds>(sync_version.time_since_epoch()).count());

    std::string notification_name = "entity_destroyed";

    // Send server entity in the request
    entity_destroy_request request{.session_id = client_id, .server_entity = server_entity, .sync_version = sync_version};

    try {
      co_await rpc_server_.notify(notification_name, std::move(request));
    } catch (std::exception const& ex) {
      spdlog::error("Error notifying entity destruction to client {}: {}", client_id, ex.what());
    } catch (...) {
      spdlog::error("Error notifying entity destruction to client {}: unknown exception", client_id);
    }
    co_return;
  }

  // Send entity destruction notification to all clients (server-initiated changes)
  asio::awaitable<void> notify_entity_destruction_to_all_clients(entity server_entity, version_type sync_version) {
    if (!notifications_enabled_) {
      co_return; // Skip notifications if disabled
    }

    std::string notification_name = "entity_destroyed";

    spdlog::debug("Notifying entity destruction to all clients: server_entity={} version={}",
                  static_cast<int>(server_entity),
                  std::chrono::duration_cast<std::chrono::milliseconds>(sync_version.time_since_epoch()).count());

    // Notify all clients with server entity ID
    for (auto& [client_id, client_state] : client_states_) {
      entity_destroy_request request{.session_id = client_id, .server_entity = server_entity, .sync_version = sync_version};
      try {
        co_await rpc_server_.notify(notification_name, std::move(request));
      } catch (std::exception const& ex) {
        spdlog::error("Error notifying entity destruction to client {}: {}", client_id, ex.what());
        // Continue with other clients
      } catch (...) {
        spdlog::error("Error notifying entity destruction to client {}: unknown exception", client_id);
        // Continue with other clients
      }
    }
    co_return;
  }

  // Send entity destruction notification to all other clients (except the one that initiated it)
  asio::awaitable<void>
  notify_entity_destruction_to_other_clients(entity server_entity, version_type sync_version, std::string const& except_client_id) {
    if (!notifications_enabled_) {
      co_return; // Skip notifications if disabled
    }

    std::string notification_name = "entity_destroyed";

    spdlog::debug("Notifying entity destruction to other clients: server_entity={} except_client={} version={}",
                  static_cast<int>(server_entity),
                  except_client_id,
                  std::chrono::duration_cast<std::chrono::milliseconds>(sync_version.time_since_epoch()).count());

    // Notify each client (except the one that made the change) with server entity ID
    for (auto& [client_id, client_state] : client_states_) {
      if (client_id == except_client_id)
        continue;

      spdlog::debug("Notifying entity destruction to client: server_entity={} client={} version={}",
                    static_cast<int>(server_entity),
                    client_id,
                    std::chrono::duration_cast<std::chrono::milliseconds>(sync_version.time_since_epoch()).count());

      // Send notification with server entity ID
      entity_destroy_request request{.session_id = client_id, .server_entity = server_entity, .sync_version = sync_version};
      try {
        co_await rpc_server_.notify(notification_name, std::move(request));
      } catch (std::exception const& ex) {
        spdlog::error("Error notifying entity destruction to client {}: {}", client_id, ex.what());
        // Continue with other clients
      } catch (...) {
        spdlog::error("Error notifying entity destruction to client {}: unknown exception", client_id);
        // Continue with other clients
      }
    }
    co_return;
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
    // Get all entities that have any of the sync components (including hierarchy)
    (collect_component_and_hierarchy_entities<SyncComponentsT>(entities), ...);

    // Remove duplicates
    std::sort(entities.begin(), entities.end());
    entities.erase(std::unique(entities.begin(), entities.end()), entities.end());

    return entities;
  }

  // Helper to save component and its hierarchy components to archive
  template <typename ComponentT>
  void save_component_and_hierarchy(cereal::PortableBinaryOutputArchive& archive) {
    // Save the component itself
    entt::snapshot{ecs_.registry()}.template get<ComponentT>(archive);

    // // Also save hierarchy components if not already a hierarchy component
    // if constexpr (!is_hierarchy_component<ComponentT>::value) {
    //   entt::snapshot{ecs_.registry()}.template get<entt_ext::parent<ComponentT>>(archive);
    //   entt::snapshot{ecs_.registry()}.template get<entt_ext::children<ComponentT>>(archive);
    // }
  }

  template <typename ComponentT>
  void collect_component_and_hierarchy_entities(std::vector<entity>& entities) {
    // Collect entities with the component itself
    collect_entities_with_component<ComponentT>(entities);

    // Also collect entities with hierarchy components if not already a hierarchy component
    // if constexpr (!is_hierarchy_component<ComponentT>::value) {
    //   collect_entities_with_component<entt_ext::parent<ComponentT>>(entities);
    //   collect_entities_with_component<entt_ext::children<ComponentT>>(entities);
    // }
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
    // Check the component itself
    bool has_comp = ecs_.template any_of<ComponentT>(entt);

    // Also check hierarchy components if not already a hierarchy component
    if constexpr (!is_hierarchy_component<ComponentT>::value) {
      has_comp = has_comp || ecs_.template any_of<entt_ext::parent<ComponentT>>(entt) || ecs_.template any_of<entt_ext::children<ComponentT>>(entt);
    }

    return has_comp;
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

    // Register entity creation endpoint
    rpc_server_.attach("entity_create", [this](entity_create_request const& request) -> asio::awaitable<entity_create_response> {
      co_return co_await handle_entity_create(request);
    });

    // Register entity destruction endpoint
    rpc_server_.attach("entity_destroy", [this](entity_destroy_request const& request) -> asio::awaitable<entity_destroy_response> {
      co_return co_await handle_entity_destroy(request);
    });

    // Register component-specific endpoints using fold expression
    (register_component_endpoints<SyncComponentsT>(ecs), ...);
  }

  template <typename ComponentT>
  void register_component_endpoints(entt_ext::ecs& ecs) {
    // Register for the component itself
    register_component_endpoints_impl<ComponentT>(ecs);

    // // Also register for hierarchy components if not already a hierarchy component
    // if constexpr (!is_hierarchy_component<ComponentT>::value) {
    //   register_component_endpoints_impl<entt_ext::parent<ComponentT>>(ecs);
    //   register_component_endpoints_impl<entt_ext::children<ComponentT>>(ecs);
    // }
  }

  template <typename ComponentT>
  void register_component_endpoints_impl(entt_ext::ecs& ecs) {
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