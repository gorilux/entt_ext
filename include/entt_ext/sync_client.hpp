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

#include <spdlog/spdlog.h>

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
    , protocol_version_(sync_component_list<SyncComponentsT...>::generate_protocol_version()) {

    // Initialize sync state if not present
    if (!ecs_.template contains<sync_state>()) {
      ecs_.template get_or_emplace<sync_state>();
    }

    // Initialize entity mapping component if not present
    if (!ecs_.template contains<entity_mapping_component>()) {
      ecs_.template get_or_emplace<entity_mapping_component>();
    }

    // Set up automatic synchronization for each component type
    (setup_automatic_sync<SyncComponentsT>(), ...);

    spdlog::info("Sync client initialized with protocol version: {}", protocol_version_);
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

    if (clear_mapping) {
      clear_entity_mapping();
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

  // Get access to entity mapping (for debugging/inspection)
  entity_mapping const& get_entity_mapping() const {
    return ecs_.template get<entity_mapping_component>().mapping;
  }

  // Clear entity mapping (useful when reconnecting)
  void clear_entity_mapping() {
    auto& mapping_component = ecs_.template get<entity_mapping_component>();
    mapping_component.mapping.clear();
  }

  // Get server entity for a client entity (returns null if not mapped)
  entity get_server_entity(entity client_entity) const {
    const auto& mapping           = ecs_.template get<entity_mapping_component>().mapping;
    auto        server_entity_opt = mapping.get_server_entity(client_entity);
    return server_entity_opt ? *server_entity_opt : entt_ext::null;
  }

  // Get client entity for a server entity (returns null if not mapped)
  entity get_client_entity(entity server_entity) const {
    const auto& mapping           = ecs_.template get<entity_mapping_component>().mapping;
    auto        client_entity_opt = mapping.get_client_entity(server_entity);
    return client_entity_opt ? *client_entity_opt : entt_ext::null;
  }

  // Check if entity is mapped to server
  bool is_entity_mapped(entity client_entity) const {
    const auto& mapping = ecs_.template get<entity_mapping_component>().mapping;
    return mapping.has_client_entity(client_entity);
  }

  // Get count of pending sync operations (useful for UI feedback)
  size_t get_pending_sync_count() const {
    size_t count      = 0;
    auto   dirty_view = ecs_.template view<dirty_client>();
    for (auto [entity] : dirty_view.each()) {
      if (has_pending_sync_changes(entity)) {
        count++;
      }
    }
    return count;
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

      // Load the snapshot into our ECS
      grlx::rpc::ibufferstream           istream(&response.snapshot_data[0], response.snapshot_data.size());
      cereal::PortableBinaryInputArchive archive(istream);

      // Use continuous loader to merge entities without conflicts
      // Note: The snapshot contains server entity IDs that get mapped to client entity IDs
      loading_snapshot_ = true;
      co_await ecs_.template merge_snapshot<cereal::PortableBinaryInputArchive, SyncComponentsT...>(archive);
      loading_snapshot_ = false;

      // Extract the entity mappings created by the continuous_loader
      // and send them to the server so it can update its client_state.mapping
      auto server_to_client_mappings = ecs_.extract_entity_mappings();
      if (!server_to_client_mappings.empty()) {
        bool mapping_synced = co_await sync_entity_mappings(server_to_client_mappings);
        if (!mapping_synced) {
          spdlog::warn("Failed to sync entity mappings to server");
        }
      }

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

    std::string component_name = std::string(type_name<ComponentT>());

    // Handle component updates
    std::string update_notification = "component_updated_" + component_name;
    rpc_client_.register_notification_handler(update_notification, [this](component_update_request<ComponentT> const& request) {
      if (request.session_id != session_id_)
        return;

      // Apply component update directly WITHOUT triggering observers (prevent loops)
      if (ecs_.valid(request.target_entity)) {
        // Use silent update to avoid triggering sync observers
        spdlog::debug("received_component_update: {} {}", type_name<ComponentT>(), static_cast<int>(request.target_entity));
        ecs_.template emplace_or_replace<component_update_request<ComponentT>>(request.target_entity, request);
      }
    });

    // Handle component removals
    std::string remove_notification = "component_removed_" + component_name;
    rpc_client_.register_notification_handler(remove_notification, [this](component_remove_request<ComponentT> const& request) {
      if (request.session_id != session_id_)
        return;

      // Remove component directly WITHOUT triggering observers (prevent loops)
      if (ecs_.valid(request.target_entity)) {
        spdlog::debug("received_component_removal: {} {}", type_name<ComponentT>(), static_cast<int>(request.target_entity));
        ecs_.template emplace_or_replace<component_remove_request<ComponentT>>(request.target_entity, request);
      }
    });
  }

  // Set up automatic sync for a specific component type
  template <typename ComponentT>
  void setup_automatic_sync() {
    // Set up component observer to track changes
    auto& observer = ecs_.component_observer<ComponentT>();

    // When a sync component is added, mark it for sync
    observer.on_construct([this](entt_ext::ecs& ecs, entt_ext::entity e, ComponentT& component) {
      if (loading_snapshot_) {
        return;
      }
      if (auto request = ecs.template try_get<component_update_request<ComponentT>>(e); request != nullptr) {
        return;
      }
      auto sync_version = std::chrono::steady_clock::now();
      ecs.template emplace_or_replace<local_component_changed<ComponentT>>(e, sync_version);
    });

    // When a sync component is updated, mark it for sync
    observer.on_update([this](entt_ext::ecs& ecs, entt_ext::entity e, ComponentT& component) {
      if (loading_snapshot_) {
        return;
      }
      spdlog::debug("Client-side component updated: {} {}", type_name<ComponentT>(), static_cast<int>(e));
      if (auto request = ecs.template try_get<component_update_request<ComponentT>>(e); request != nullptr) {
        return;
      }
      auto sync_version = std::chrono::steady_clock::now();
      ecs.template emplace_or_replace<local_component_changed<ComponentT>>(e, sync_version);
    });

    // When a sync component is removed, mark it for removal sync
    observer.on_destroy([this](entt_ext::ecs& ecs, entt_ext::entity e, ComponentT& component) {
      if (loading_snapshot_) {
        return;
      }
      if (auto request = ecs.template try_get<component_remove_request<ComponentT>>(e); request != nullptr) {
        return;
      }
      spdlog::debug("Client-side component destroyed: {} {}", type_name<ComponentT>(), static_cast<int>(e));
      auto sync_version = std::chrono::steady_clock::now();
      asio::co_spawn(
          ecs.concurrent_io_context(), // or rpc_client_.get_executor()
          [this, e, sync_version]() -> asio::awaitable<void> {
            co_await notify_component_removal<ComponentT>(e, sync_version);
          },
          asio::detached);
    });

    // Set up system to process component changes
    ecs_.system<local_component_changed<ComponentT>, ComponentT>(entt::exclude<component_update_request<ComponentT>>)
        .each(
            [this](entt_ext::ecs&                       ecs,
                   entt_ext::system&                    self,
                   double                               dt,
                   entt_ext::entity                     e,
                   local_component_changed<ComponentT>& sync_data,
                   ComponentT&                          component) -> asio::awaitable<void> {
              try {

                // Send this specific component to server
                co_await send_component_to_server<ComponentT>(e, component, sync_data.sync_version);

                co_await ecs.template remove_deferred<local_component_changed<ComponentT>>(e);

              } catch (std::exception const& e) {
                spdlog::error("Error sending component to server: {}", e.what());
              } catch (...) {
                // On error, keep the marker for retry
                spdlog::error("Error sending component to server: unknown exception");
              }

              co_return;
            },
            entt_ext::run_policy_t<entt_ext::run_policy::detached>{})
        .stage(entt_ext::stage::update);

    ecs_.system<component_update_request<ComponentT>, ComponentT>(entt::exclude<local_component_changed<ComponentT>>)
        .each(
            [this](entt_ext::ecs&                        ecs,
                   entt_ext::system&                     self,
                   double                                dt,
                   entt_ext::entity                      e,
                   component_update_request<ComponentT>& request,
                   ComponentT&                           component) -> asio::awaitable<void> {
              // Notify all clients about the component update
              co_await ecs.template replace_deferred<ComponentT>(e, request.component_data);
              co_await ecs.template remove_deferred<component_update_request<ComponentT>>(e);
              co_return;
            },
            entt_ext::run_policy_t<entt_ext::run_policy::detached>{})
        .stage(entt_ext::stage::update);

    ecs_.system<component_update_request<ComponentT>>(entt::exclude<ComponentT>)
        .each(
            [this](entt_ext::ecs& ecs, entt_ext::system& self, double dt, entt_ext::entity e, component_update_request<ComponentT>& request)
                -> asio::awaitable<void> {
              // Notify all clients about the component update
              co_await ecs.template emplace_deferred<ComponentT>(e, request.component_data);
              co_await ecs.template remove_deferred<component_update_request<ComponentT>>(e);
              co_return;
            },
            entt_ext::run_policy_t<entt_ext::run_policy::detached>{})
        .stage(entt_ext::stage::update);

    ecs_.system<component_remove_request<ComponentT>>(entt::exclude<local_component_removed<ComponentT>>)
        .each(
            [this](entt_ext::ecs& ecs, entt_ext::system& self, double dt, entt_ext::entity e, component_remove_request<ComponentT>& request)
                -> asio::awaitable<void> {
              co_await ecs.template remove_deferred<ComponentT>(e);
              co_await ecs.template remove_deferred<component_remove_request<ComponentT>>(e);
              co_return;
            },
            entt_ext::run_policy_t<entt_ext::run_policy::detached>{})
        .stage(entt_ext::stage::update);

    ecs_.system<local_component_removed<ComponentT>>(entt::exclude<component_remove_request<ComponentT>>)
        .each(
            [this](entt_ext::ecs& ecs, entt_ext::system& self, double dt, entt_ext::entity e, local_component_removed<ComponentT>& sync_data)
                -> asio::awaitable<void> {
              co_await notify_component_removal<ComponentT>(e, sync_data.sync_version);

              co_await ecs.template remove_deferred<local_component_removed<ComponentT>>(e);
              co_return;
            },
            entt_ext::run_policy_t<entt_ext::run_policy::detached>{})
        .stage(entt_ext::stage::update);
  }

  // Send a specific component to the server using type-safe endpoints (insert or update)
  template <typename ComponentT>
  asio::awaitable<void> send_component_to_server(entity e, ComponentT& component, version_type sync_version) {
    spdlog::debug("Sending component to server: {} {} {}",
                  type_name<ComponentT>(),
                  static_cast<int>(e),
                  std::chrono::duration_cast<std::chrono::milliseconds>(sync_version.time_since_epoch()).count());
    std::string endpoint_name = "component_updated_" + std::string(type_name<ComponentT>());

    component_update_request<ComponentT> request{.session_id     = session_id_,
                                                 .sync_version   = sync_version,
                                                 .target_entity  = e,
                                                 .component_data = component};

    auto response = co_await rpc_client_.template invoke<component_update_response<ComponentT>>(endpoint_name, std::move(request));

    if (!response.success) {
      spdlog::error("Component sync failed: {}", response.error_message);
      throw std::runtime_error("Component sync failed: " + response.error_message);
    }

    // Update entity mapping if we got a server entity back
    if (response.server_entity != entt_ext::null) {
      auto& mapping_component = ecs_.template get<entity_mapping_component>();
      mapping_component.mapping.add_mapping(e, response.server_entity);
    }
    spdlog::debug("Component sent to server: {} {} {}",
                  type_name<ComponentT>(),
                  static_cast<int>(e),
                  std::chrono::duration_cast<std::chrono::milliseconds>(sync_version.time_since_epoch()).count());

    co_return;
  }

  // Notify server about component removal using type-safe endpoints
  template <typename ComponentT>
  asio::awaitable<void> notify_component_removal(entity e, version_type sync_version) {
    std::string endpoint_name = "component_removed_" + std::string(type_name<ComponentT>());

    component_remove_request<ComponentT> request{.session_id = session_id_, .sync_version = sync_version, .target_entity = e};

    auto response = co_await rpc_client_.template invoke<component_remove_response<ComponentT>>(endpoint_name, std::move(request));

    if (!response.success) {
      throw std::runtime_error("Component removal sync failed: " + response.error_message);
    }
    co_return;
  }

  // Sync entity mappings to server
  asio::awaitable<bool> sync_entity_mappings(std::unordered_map<entity, entity> const& server_to_client_mappings) {
    try {
      if (!rpc_client_.is_connected() || session_id_.empty()) {
        co_return false;
      }

      entity_mapping_update_request request{.session_id = session_id_, .server_to_client_mapping = server_to_client_mappings};

      auto response = co_await rpc_client_.template invoke<entity_mapping_update_response>("entity_mapping_update", std::move(request));

      if (!response.success) {
        spdlog::error("Entity mapping sync failed: {}", response.error_message);
        co_return false;
      }

      spdlog::debug("Successfully synced {} entity mappings to server", server_to_client_mappings.size());
      co_return true;

    } catch (std::exception const& ex) {
      spdlog::error("Exception during entity mapping sync: {}", ex.what());
      co_return false;
    } catch (...) {
      spdlog::error("Unknown exception during entity mapping sync");
      co_return false;
    }
  }

  // Check if entity has any pending sync changes
  bool has_pending_sync_changes(entity e) const {
    return (ecs_.template any_of<local_component_changed<SyncComponentsT>>(e) || ...) ||
           (ecs_.template any_of<local_component_removed<SyncComponentsT>>(e) || ...);
  }

  // Perform handshake with server to get session ID
  asio::awaitable<bool> perform_handshake(std::string const& client_name, std::string const& client_version) {
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

private:
  ecs&        ecs_;
  rpc_client  rpc_client_;
  std::string session_id_;       // Session ID obtained from handshake
  std::string protocol_version_; // Protocol version based on component types
  bool        loading_snapshot_ = false;
};

} // namespace entt_ext::sync