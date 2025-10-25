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
    , protocol_version_(sync_component_list<SyncComponentsT...>::generate_protocol_version())
    , continuous_loader_(ecs_.registry()) {

    // Initialize sync state if not present
    if (!ecs_.template contains<sync_state>()) {
      ecs_.template get_or_emplace<sync_state>();
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

      ecs_.defer([this, response](entt_ext::ecs& ecs) -> asio::awaitable<void> {
        // Load the snapshot into our ECS
        // Use continuous loader to merge entities without conflicts
        // Note: The snapshot contains server entity IDs that get mapped to client entity IDs
        loading_snapshot_ = true;
        grlx::rpc::ibufferstream           istream(&response.snapshot_data[0], response.snapshot_data.size());
        cereal::PortableBinaryInputArchive archive(istream);
        // load entities
        continuous_loader_.get<entt_ext::entity>(archive);

        // Load components
        (continuous_loader_.template get<SyncComponentsT>(archive), ...);

        continuous_loader_.orphans();

        loading_snapshot_ = false;

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
        return; // Entity not mapped yet, ignore
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

  // Request a server entity for a client entity
  asio::awaitable<entity> request_server_entity(entity client_entity) {
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
  asio::awaitable<void> send_component_to_server(entity e, ComponentT& component, version_type sync_version) {
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

    std::string endpoint_name = "component_updated_" + std::string(type_name<ComponentT>());

    // target_entity is now the server entity
    component_update_request<ComponentT> request{.session_id     = session_id_,
                                                 .sync_version   = sync_version,
                                                 .target_entity  = server_entity,
                                                 .component_data = component};

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
  asio::awaitable<void> notify_component_removal(entity e, version_type sync_version) {
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
  ecs&                                           ecs_;
  rpc_client                                     rpc_client_;
  std::string                                    session_id_;       // Session ID obtained from handshake
  std::string                                    protocol_version_; // Protocol version based on component types
  bool                                           loading_snapshot_ = false;
  continuous_loader_with_mapping<entt::registry> continuous_loader_;
};

} // namespace entt_ext::sync