#pragma once

#include "ecs.hpp"
#include "entity_mapping.hpp"
#include "sync_common.hpp"
#include "type_name.hpp"

#include <grlx/rpc/client.hpp>
#include <grlx/rpc/encoder.hpp>
#include <grlx/rpc/message.hpp>
#include <grlx/rpc/tcp_channel.hpp>

#include <cereal/archives/portable_binary.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/unordered_map.hpp>
#include <cereal/types/unordered_set.hpp>
#include <cereal/types/vector.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/use_awaitable.hpp>

// #include <spdlog/spdlog.h>

#include <chrono>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace entt_ext::sync {

namespace asio = boost::asio;

// Client-side synchronization manager
template <typename... SyncComponentsT>
class sync_client {
  using channel_type = grlx::rpc::tcp_channel<grlx::rpc::binary_encoder>;
  using rpc_client   = grlx::rpc::client<channel_type>;
  using tcp          = boost::asio::ip::tcp;

public:
  explicit sync_client(ecs& ecs_instance)
    : ecs_(ecs_instance)
    , protocol_version_(sync_component_list<SyncComponentsT...>::generate_protocol_version())
    //, protocol_version_("sync_v1_")
    , continuous_loader_(ecs_.registry()) {

    // Initialize sync state if not present
    if (!ecs_.template contains<sync_state>()) {
      ecs_.template get_or_emplace<sync_state>();
    }
    // Set up automatic synchronization for each component type
    (setup_automatic_sync<SyncComponentsT>(), ...);
    setup_entity_sync();

    // spdlog::info("Sync client initialized with protocol version: {}", protocol_version_);
  }

  // Connect to the sync server and perform handshake
  asio::awaitable<bool>
  connect(std::string const& host, std::uint16_t port, std::string const& client_name = "", std::string const& client_version = "") {
    try {
      // First establish TCP connection
      auto          executor = co_await asio::this_coro::executor;
      tcp::resolver resolver(executor);
      auto          endpoints = co_await resolver.async_resolve(host, std::to_string(port), asio::use_awaitable);
      co_await rpc_client_.connect(*endpoints.begin());

      // Set up notification handlers for real-time sync
      setup_notification_handlers();

      // Perform handshake to get session ID
      bool handshake_success = co_await perform_handshake(client_name, client_version);
      if (!handshake_success) {
        // Disconnect on handshake failure
        co_await disconnect();
        co_return false;
      }

      co_await request_snapshot();

      co_return true;
    } catch (...) {
      session_id_.clear(); // Clear session on connection failure
      co_return false;
    }
  }

  // Disconnect from the sync server
  asio::awaitable<void> disconnect(bool clear_mapping = false) {
    try {
      rpc_client_.disconnect();
    } catch (...) {
      // Ignore disconnect errors
    }
    session_id_.clear(); // Clear session on disconnect

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

  // Request ECS snapshot from server
  asio::awaitable<bool> request_snapshot(std::vector<entity> const& entities_of_interest = {}) {
    if (!rpc_client_.is_connected()) {
      co_return false;
    }

    try {
      sync_request request{.session_id           = session_id_,
                           .client_timestamp     = std::chrono::steady_clock::now(),
                           .entities_of_interest = entities_of_interest};

      auto response = co_await rpc_client_.template invoke<sync_response>("sync", std::move(request));

      // Apply the received snapshot
      bool success = co_await apply_sync_response(response);
      co_return success;

    } catch (...) {
      co_return false;
    }
  }

  // Connect to server and request initial snapshot in one operation
  asio::awaitable<bool> connect_and_sync(std::string const&         host,
                                         std::uint16_t              port,
                                         std::vector<entity> const& entities_of_interest = {},
                                         std::string const&         client_name          = "",
                                         std::string const&         client_version       = "") {
    bool connected = co_await connect(host, port, client_name, client_version);
    if (!connected) {
      co_return false;
    }

    bool synced = co_await request_snapshot(entities_of_interest);
    co_return synced;
  }

  // Force sync all pending changes (useful after reconnection)
  asio::awaitable<void> force_sync_all() {
    // This will cause all systems to process their pending markers
    // when the next frame runs (assuming we're connected)
    co_return;
  }

  // Apply synchronized state from server
  asio::awaitable<bool> apply_sync_response(sync_response const& response) {
    try {
      if (response.snapshot_data.empty()) {
        co_return false;
      }

      ecs_.defer_async([this, response](entt_ext::ecs& ecs) -> asio::awaitable<void> {
        // Load the snapshot into our ECS
        // Use continuous loader to merge entities without conflicts
        // Note: The snapshot contains server entity IDs that get mapped to client entity IDs
        spdlog::debug("Loading snapshot from server");
        loading_snapshot_ = true;
        grlx::rpc::ibufferstream           istream(&response.snapshot_data[0], response.snapshot_data.size());
        cereal::PortableBinaryInputArchive archive(istream);
        // load entities
        continuous_loader_.get<entt_ext::entity>(archive);

        // Load components (including hierarchy components)
        (load_component_and_hierarchy<SyncComponentsT>(archive), ...);

        continuous_loader_.orphans();

        // Remap entity references inside components after loading (including hierarchy components)
        (co_await remap_component_and_hierarchy<SyncComponentsT>(), ...);

        ecs_.defer_async([this](entt_ext::ecs&) -> asio::awaitable<void> {
          loading_snapshot_ = false;
          spdlog::debug("Snapshot loaded from server");
          co_return;
        });

        co_return;
      });

      // Update sync state
      auto& state     = ecs_.template get<sync_state>();
      state.last_sync = response.server_timestamp;

      co_return true;

    } catch (...) {
      loading_snapshot_ = false;
      co_return false;
    }
  }

private:
  // Set up notification handlers for real-time sync updates
  void setup_notification_handlers() {
    // Register handlers for component-specific notifications
    (setup_component_notification_handlers<SyncComponentsT>(), ...);
  }

  // Set up notification handlers for a specific component type
  template <typename ComponentT>
  void setup_component_notification_handlers() {
    // Set up for the component itself
    setup_component_notification_handlers_impl<ComponentT>();

    // // Also set up for hierarchy components if not already a hierarchy component
    // if constexpr (!is_hierarchy_component<ComponentT>::value) {
    //   setup_component_notification_handlers_impl<entt_ext::parent<ComponentT>>();
    //   setup_component_notification_handlers_impl<entt_ext::children<ComponentT>>();
    // }
  }

  // Implementation of notification handler setup for a single component
  template <typename ComponentT>
  void setup_component_notification_handlers_impl() {

    std::string component_name = std::string(type_name<ComponentT>());

    // Handle component updates
    std::string update_notification = "component_updated_" + component_name;
    rpc_client_.register_notification_handler(update_notification, [this](component_update_request<ComponentT> const& request) {
      if (request.session_id != session_id_)
        return;

      // request.target_entity contains the server entity
      entity server_entity = request.target_entity;

      // Map server entity to local client entity
      auto client_entity = continuous_loader_.to_local(server_entity);

      if (client_entity == entt_ext::null) {
        spdlog::debug("Received component update for unmapped server entity {} ({})", static_cast<int>(server_entity), type_name<ComponentT>());
        client_entity = ecs_.create();
        continuous_loader_.insert_mapping(server_entity, client_entity);
        spdlog::debug("Created new client entity {} for server entity {}", static_cast<int>(client_entity), static_cast<int>(server_entity));
      }

      // Apply component update directly WITHOUT triggering observers (prevent loops)
      if (ecs_.valid(client_entity)) {
        spdlog::debug("received_component_update: {} server={} client={}",
                      type_name<ComponentT>(),
                      static_cast<int>(server_entity),
                      static_cast<int>(client_entity));
        ecs_.template emplace_or_replace<component_update_request<ComponentT>>(client_entity, request);
      }
    });

    // Handle component removals
    std::string remove_notification = "component_removed_" + component_name;
    rpc_client_.register_notification_handler(remove_notification, [this](component_remove_request<ComponentT> const& request) {
      if (request.session_id != session_id_)
        return;

      // request.target_entity contains the server entity
      entity server_entity = request.target_entity;

      // Map server entity to local client entity
      auto client_entity = continuous_loader_.to_local(server_entity);

      if (client_entity == entt_ext::null) {
        spdlog::debug("Received component removal for unmapped server entity {} ({})", static_cast<int>(server_entity), type_name<ComponentT>());
        return; // Entity not mapped yet, ignore
      }

      // Remove component directly WITHOUT triggering observers (prevent loops)
      if (ecs_.valid(client_entity)) {
        spdlog::debug("received_component_removal: {} server={} client={}",
                      type_name<ComponentT>(),
                      static_cast<int>(server_entity),
                      static_cast<int>(client_entity));
        ecs_.template emplace_or_replace<component_remove_request<ComponentT>>(client_entity, request);
      }
    });
  }

  // Set up automatic sync for a specific component type
  template <typename ComponentT>
  void setup_automatic_sync() {
    // Set up for the component itself
    setup_automatic_sync_impl<ComponentT>();

    // // Also set up for hierarchy components if not already a hierarchy component
    // if constexpr (!is_hierarchy_component<ComponentT>::value) {
    //   setup_automatic_sync_impl<entt_ext::parent<ComponentT>>();
    //   setup_automatic_sync_impl<entt_ext::children<ComponentT>>();
    // }
  }

  // Implementation of automatic sync setup for a single component
  template <typename ComponentT>
  void setup_automatic_sync_impl() {
    // Set up component observer to track changes
    auto& observer = ecs_.component_observer<ComponentT>();

    // When a sync component is added, send it to server immediately
    observer.on_construct([this](entt_ext::ecs& ecs, entt_ext::entity e, ComponentT& component) -> asio::awaitable<void> {
      spdlog::debug("Client-side component added: {} {}", type_name<ComponentT>(), static_cast<int>(e));

      if (loading_snapshot_) {
        co_return;
      }
      if (auto request = ecs.template try_get<component_update_request<ComponentT>>(e); request != nullptr) {
        co_return;
      }

      auto sync_version = std::chrono::steady_clock::now();

      try {
        co_await send_component_to_server<ComponentT>(e, component, sync_version);
      } catch (std::exception const& ex) {
        spdlog::error("Error sending component to server: {}", ex.what());
      } catch (...) {
        spdlog::error("Error sending component to server: unknown exception");
      }

      co_return;
    });

    // When a sync component is updated, send it to server immediately
    observer.on_update([this](entt_ext::ecs& ecs, entt_ext::entity e, ComponentT& component) -> asio::awaitable<void> {
      spdlog::debug("Client-side component updated: {} {}", type_name<ComponentT>(), static_cast<int>(e));

      if (loading_snapshot_) {
        co_return;
      }
      if (auto request = ecs.template try_get<component_update_request<ComponentT>>(e); request != nullptr) {
        co_return;
      }

      auto sync_version = std::chrono::steady_clock::now();

      try {
        co_await send_component_to_server<ComponentT>(e, component, sync_version);
      } catch (std::exception const& ex) {
        spdlog::error("Error sending component to server: {}", ex.what());
      } catch (...) {
        spdlog::error("Error sending component to server: unknown exception");
      }

      co_return;
    });

    // When a sync component is removed, notify server immediately
    observer.on_destroy([this](entt_ext::ecs& ecs, entt_ext::entity e, ComponentT& component) -> asio::awaitable<void> {
      if (loading_snapshot_) {
        co_return;
      }
      if (auto request = ecs.template try_get<component_remove_request<ComponentT>>(e); request != nullptr) {
        co_return;
      }

      spdlog::debug("Client-side component destroyed: {} {}", type_name<ComponentT>(), static_cast<int>(e));
      auto sync_version = std::chrono::steady_clock::now();

      try {
        co_await notify_component_removal<ComponentT>(e, sync_version);
      } catch (std::exception const& ex) {
        spdlog::error("Error notifying component removal to server: {}", ex.what());
      } catch (...) {
        spdlog::error("Error notifying component removal to server: unknown exception");
      }

      co_return;
    });

    // Set up observer for component_update_request to apply updates from server
    auto& update_request_observer = ecs_.component_observer<component_update_request<ComponentT>>();

    update_request_observer.on_construct(
        [this](entt_ext::ecs& ecs, entt_ext::entity e, component_update_request<ComponentT>& request) -> asio::awaitable<void> {
          try {
            // Map entity references from remote to local IDs
            auto component_data = request.component_data;
            if constexpr (is_hierarchy_component<ComponentT>::value) {
              co_await map_entities_async(component_data);
            }

            // Apply the component update
            ecs.template emplace_or_replace<ComponentT>(e, component_data);
            // Note: Do NOT remove the request marker here - we need it to persist
            // until after all async on_update handlers have had a chance to check it.
            // Use a separate deferred removal that runs after all current handlers complete.
            ecs.defer([e](entt_ext::ecs& ecs_ref) {
              ecs_ref.template remove<component_update_request<ComponentT>>(e);
            });
          } catch (std::exception const& ex) {
            spdlog::error("Error applying component update request: {}", ex.what());
          } catch (...) {
            spdlog::error("Error applying component update request: unknown exception");
          }

          co_return;
        });

    // Set up observer for component_remove_request to apply removals from server
    auto& remove_request_observer = ecs_.component_observer<component_remove_request<ComponentT>>();

    remove_request_observer.on_construct(
        [this](entt_ext::ecs& ecs, entt_ext::entity e, component_remove_request<ComponentT>& request) -> asio::awaitable<void> {
          try {
            ecs.template remove<ComponentT>(e);
            co_await ecs.template remove_deferred<component_remove_request<ComponentT>>(e);
          } catch (std::exception const& ex) {
            spdlog::error("Error applying component remove request: {}", ex.what());
          } catch (...) {
            spdlog::error("Error applying component remove request: unknown exception");
          }

          co_return;
        });
  }

  // Request a server entity for a client entity
  auto request_server_entity(entity client_entity) -> asio::awaitable<entity> {
    if (!rpc_client_.is_connected() || session_id_.empty()) {
      co_return entt_ext::null;
    }

    try {
      entity_create_request request{.session_id = session_id_, .client_entity = client_entity};

      auto response = co_await rpc_client_.template invoke<entity_create_response>("entity_create", std::move(request));

      if (!response.success) {
        spdlog::error("Entity creation failed: {}", response.error_message);
        co_return entt_ext::null;
      }

      continuous_loader_.insert_mapping(response.server_entity, client_entity);

      spdlog::debug("Requested server entity {} for client entity {}", static_cast<int>(response.server_entity), static_cast<int>(client_entity));

      co_return response.server_entity;

    } catch (std::exception const& ex) {
      spdlog::error("Exception requesting server entity: {}", ex.what());
      co_return entt_ext::null;
    }
  }

  // Send a specific component to the server using type-safe endpoints (insert or update)
  template <typename ComponentT>
  auto send_component_to_server(entity e, ComponentT& component, version_type sync_version) -> asio::awaitable<void> {
    // Get or request server entity for this client entity
    auto server_entity = continuous_loader_.to_remote(e);

    if (server_entity == entt_ext::null) {
      // Request a new server entity
      server_entity = co_await request_server_entity(e);
      if (server_entity == entt_ext::null) {
        spdlog::error("Failed to get server entity for client entity {}", static_cast<int>(e));
        throw std::runtime_error("Failed to get server entity");
      }
    }

    spdlog::debug("Sending component to server: {} client={} server={} {}",
                  type_name<ComponentT>(),
                  static_cast<int>(e),
                  static_cast<int>(server_entity),
                  std::chrono::duration_cast<std::chrono::milliseconds>(sync_version.time_since_epoch()).count());

    // Create a copy of the component for mapping
    ComponentT component_to_send = component;

    if constexpr (is_hierarchy_component<ComponentT>::value) {
      co_await map_component_entities_to_remote_async(component_to_send);
    }

    std::string endpoint_name = "component_updated_" + std::string(type_name<ComponentT>());

    // target_entity is now the server entity
    component_update_request<ComponentT> request{.session_id     = session_id_,
                                                 .sync_version   = sync_version,
                                                 .target_entity  = server_entity,
                                                 .component_data = component_to_send};

    auto response = co_await rpc_client_.template invoke<component_update_response<ComponentT>>(endpoint_name, std::move(request));

    if (!response.success) {
      spdlog::error("Component sync failed: {}", response.error_message);
      throw std::runtime_error("Component sync failed: " + response.error_message);
    }

    spdlog::debug("Component sent to server: {} client={} server={} {}",
                  type_name<ComponentT>(),
                  static_cast<int>(e),
                  static_cast<int>(server_entity),
                  std::chrono::duration_cast<std::chrono::milliseconds>(sync_version.time_since_epoch()).count());

    co_return;
  }

  // Notify server about component removal using type-safe endpoints
  template <typename ComponentT>
  auto notify_component_removal(entity e, version_type sync_version) -> asio::awaitable<void> {
    // Get server entity for this client entity
    auto server_entity = continuous_loader_.map(e);

    if (server_entity == entt_ext::null) {
      // No server entity mapping exists, nothing to remove on server
      spdlog::debug("No server entity mapping for client entity {} during component removal", static_cast<int>(e));
      co_return;
    }

    std::string endpoint_name = "component_removed_" + std::string(type_name<ComponentT>());

    // target_entity is now the server entity
    component_remove_request<ComponentT> request{.session_id = session_id_, .sync_version = sync_version, .target_entity = server_entity};

    auto response = co_await rpc_client_.template invoke<component_remove_response<ComponentT>>(endpoint_name, std::move(request));

    if (!response.success) {
      throw std::runtime_error("Component removal sync failed: " + response.error_message);
    }
    co_return;
  }

  // Notify server about entity destruction
  auto notify_entity_destruction_to_server(entity e, version_type sync_version) -> asio::awaitable<void> {

    if (loading_snapshot_) {
      spdlog::debug("Skipping entity destruction notification for client entity {} during snapshot load", static_cast<int>(e));
      co_return;
    }
    // Get server entity for this client entity
    auto server_entity = continuous_loader_.to_remote(e);

    if (server_entity == entt_ext::null) {
      // No server entity mapping exists, nothing to destroy on server
      spdlog::debug("No server entity mapping for client entity {} during entity destruction", static_cast<int>(e));
      co_return;
    }

    if (!rpc_client_.is_connected() || session_id_.empty()) {
      spdlog::debug("Not connected to server, skipping entity destruction notification for client entity {}", static_cast<int>(e));
      co_return;
    }

    spdlog::debug("Notifying server about entity destruction: client={} server={}", static_cast<int>(e), static_cast<int>(server_entity));

    entity_destroy_request request{.session_id = session_id_, .server_entity = server_entity, .sync_version = sync_version};

    auto response = co_await rpc_client_.template invoke<entity_destroy_response>("entity_destroy", std::move(request));

    if (!response.success) {
      spdlog::error("Entity destruction sync failed: {}", response.error_message);
      throw std::runtime_error("Entity destruction sync failed: " + response.error_message);
    }

    spdlog::debug("Entity destruction notification sent successfully: client={} server={}", static_cast<int>(e), static_cast<int>(server_entity));
    co_return;
  }

  // Helper to load component and its hierarchy components from archive
  template <typename ComponentT>
  void load_component_and_hierarchy(cereal::PortableBinaryInputArchive& archive) {
    // Load the component itself
    continuous_loader_.template get<ComponentT>(archive);

    // Also load hierarchy components if not already a hierarchy component
    // if constexpr (!is_hierarchy_component<ComponentT>::value) {
    //   continuous_loader_.template get<entt_ext::parent<ComponentT>>(archive);
    //   continuous_loader_.template get<entt_ext::children<ComponentT>>(archive);
    // }
  }

  // Helper to remap component and its hierarchy components
  template <typename ComponentT>
  auto remap_component_and_hierarchy() -> asio::awaitable<void> {
    if constexpr (is_hierarchy_component<ComponentT>::value) {
      co_await remap_component_entities<ComponentT>();
    }
    // else {
    //   co_await remap_component_entities<entt_ext::parent<ComponentT>>();
    //   co_await remap_component_entities<entt_ext::children<ComponentT>>();
    // }
    co_return;
  }

  // Helper to remap entity references in components after loading from snapshot
  template <typename ComponentT>
  auto remap_component_entities() -> asio::awaitable<void> {
    auto view = ecs_.view<ComponentT>();
    for (auto entity : view) {
      auto& component = view.template get<ComponentT>(entity);
      co_await map_entities_async(component);
    }
    co_return;
  }

  template <typename Type>
  auto map_entities_async(parent<Type>& parent) -> asio::awaitable<void> {
    auto local_entity = continuous_loader_.map(parent.entity);
    if (local_entity == entt_ext::null) {
      local_entity = ecs_.create();
      continuous_loader_.insert_mapping(parent.entity, local_entity);
    }
    parent.entity = local_entity;
    co_return;
  }

  template <typename Type>
  auto map_entities_async(children<Type>& child_set) -> asio::awaitable<void> {
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

  // Helper to map entity references to remote (server) IDs before sending
  // If any referenced entities don't have server mappings, request them first
  template <typename ComponentT>
  auto map_component_entities_to_remote_async(parent<ComponentT>& component) -> asio::awaitable<void> {
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
  auto map_component_entities_to_remote_async(children<ComponentT>& component) -> asio::awaitable<void> {
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

  // Perform handshake with server to get session ID
  auto perform_handshake(std::string const& client_name, std::string const& client_version) -> asio::awaitable<bool> {
    try {
      handshake_request request{.client_name = client_name, .client_version = client_version, .protocol_version = protocol_version_};

      auto response = co_await rpc_client_.template invoke<handshake_response>("handshake", std::move(request));

      if (response.success) {
        // Validate protocol version match
        if (!response.protocol_version.empty() && response.protocol_version != protocol_version_) {
          spdlog::error("Protocol version mismatch! Client: {}, Server: {}", protocol_version_, response.protocol_version);
          session_id_.clear();
          co_return false;
        }

        session_id_ = response.session_id;
        spdlog::info("Handshake successful - Session: {}, Protocol: {}", session_id_, protocol_version_);
        co_return true;
      } else {
        spdlog::error("Handshake failed: {}", response.error_message);
        session_id_.clear();
        co_return false;
      }

    } catch (std::exception const& ex) {
      spdlog::error("Handshake exception: {}", ex.what());
      session_id_.clear();
      co_return false;
    } catch (...) {
      spdlog::error("Handshake unknown exception");
      session_id_.clear();
      co_return false;
    }
  }

  void setup_entity_sync() {
    auto& observer = ecs_.component_observer<entt_ext::entity>();
    observer.on_destroy([this](entt_ext::ecs& ecs, entt_ext::entity e) -> asio::awaitable<void> {
      if (continuous_loader_.contains_local(e)) {
        spdlog::debug("Entity destroyed, notifying server and removing mapping: {}", static_cast<int>(e));

        auto sync_version = std::chrono::steady_clock::now();

        try {
          co_await notify_entity_destruction_to_server(e, sync_version);
        } catch (std::exception const& ex) {
          spdlog::error("Error notifying entity destruction to server: {} {}", static_cast<int>(e), ex.what());
        } catch (...) {
          spdlog::error("Error notifying entity destruction to server: unknown exception");
        }

        // Remove entity mapping after notifying server
        continuous_loader_.remove_mapping_by_local(e);
      }
      co_return;
    });
  }

private:
  ecs&                                           ecs_;
  rpc_client                                     rpc_client_;
  std::string                                    session_id_;       // Session ID obtained from handshake
  std::string                                    protocol_version_; // Protocol version based on component types
  bool                                           loading_snapshot_ = false;
  continuous_loader_with_mapping<entt::registry> continuous_loader_;
};

} // namespace entt_ext::sync