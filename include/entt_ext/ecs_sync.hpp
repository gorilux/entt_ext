#pragma once

#include "ecs.hpp"
#include "type_name.hpp"

#include <grlx/rpc/encoder.hpp>
#include <grlx/rpc/message.hpp>

#include <cereal/archives/portable_binary.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/unordered_set.hpp>
#include <cereal/types/vector.hpp>

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <unordered_set>
#include <vector>

namespace entt_ext::sync {

namespace asio = boost::asio;

// Synchronization message types
struct sync_request {
  std::chrono::steady_clock::time_point client_timestamp;
  std::vector<entity>                   entities_of_interest; // Client can specify which entities to sync

  template <typename Archive>
  void serialize(Archive& archive) {
    archive(client_timestamp, entities_of_interest);
  }
};

struct sync_response {
  std::chrono::steady_clock::time_point server_timestamp;
  grlx::rpc::buffer_type                snapshot_data;
  std::vector<entity>                   synchronized_entities;

  template <typename Archive>
  void serialize(Archive& archive) {
    archive(server_timestamp, snapshot_data, synchronized_entities);
  }
};

struct component_add_request {
  entity                 target_entity;
  std::string            component_type_name;
  grlx::rpc::buffer_type component_data;

  template <typename Archive>
  void serialize(Archive& archive) {
    archive(target_entity, component_type_name, component_data);
  }
};

struct component_add_response {
  bool        success;
  std::string error_message;
  entity      resulting_entity; // May be different if entity was created

  template <typename Archive>
  void serialize(Archive& archive) {
    archive(success, error_message, resulting_entity);
  }
};

// Tag components for sync management
struct sync_enabled {};     // Mark entities that should be synchronized
struct client_owned {};     // Mark entities that originated from client
struct server_authority {}; // Mark components that only server can modify

// Sync state tracking
struct sync_state {
  std::chrono::steady_clock::time_point last_sync;
  std::unordered_set<entity>            dirty_entities;
  std::unordered_set<std::string>       synchronized_component_types;

  template <typename Archive>
  void serialize(Archive& archive) {
    archive(last_sync, dirty_entities, synchronized_component_types);
  }
};

// Component factory for dynamic component creation
using component_factory_func = std::function<bool(ecs&, entity, grlx::rpc::buffer_type const&)>;

class component_registry {
public:
  template <typename ComponentT>
  void register_component() {
    auto type_name_str = std::string(type_name<ComponentT>());

    factories_[type_name_str] = [](ecs& ecs_instance, entity entt, grlx::rpc::buffer_type const& data) -> bool {
      try {
        grlx::rpc::ibufferstream           istream(&data[0], data.size());
        cereal::PortableBinaryInputArchive archive(istream);

        ComponentT component;
        archive(component);

        ecs_instance.emplace_or_replace<ComponentT>(entt, std::move(component));
        return true;
      } catch (...) {
        return false;
      }
    };
  }

  bool create_component(std::string const& type_name, ecs& ecs_instance, entity entt, grlx::rpc::buffer_type const& data) {
    auto it = factories_.find(type_name);
    if (it != factories_.end()) {
      return it->second(ecs_instance, entt, data);
    }
    return false;
  }

  std::vector<std::string> get_registered_types() const {
    std::vector<std::string> types;
    types.reserve(factories_.size());
    for (auto const& [type_name, _] : factories_) {
      types.push_back(type_name);
    }
    return types;
  }

private:
  std::unordered_map<std::string, component_factory_func> factories_;
};

// Server-side synchronization manager
template <typename... SyncComponentsT>
class sync_server {
public:
  explicit sync_server(ecs& ecs_instance)
    : ecs_(ecs_instance) {
    // Register all synchronizable components
    (component_registry_.template register_component<SyncComponentsT>(), ...);

    // Initialize sync state if not present
    if (!ecs_.template contains<sync_state>()) {
      ecs_.template get_or_emplace<sync_state>();
    }
  }

  // RPC endpoint for synchronization
  asio::awaitable<sync_response> handle_sync_request(sync_request const& request) {
    auto& state = ecs_.template get<sync_state>();

    // Create snapshot of requested entities with sync-enabled components
    grlx::rpc::buffer_type snapshot_buffer;
    std::vector<entity>    synchronized_entities;

    try {
      grlx::rpc::ovectorstream            ostream;
      cereal::PortableBinaryOutputArchive archive(ostream);

      // If specific entities requested, use those; otherwise sync all sync_enabled entities
      auto entities_to_sync = request.entities_of_interest.empty() ? get_sync_enabled_entities() : request.entities_of_interest;

      // Create a sub-snapshot with only the requested entities
      auto temp_registry = entt::registry{};

      for (auto entt : entities_to_sync) {
        if (ecs_.valid(entt) && ecs_.template any_of<sync_enabled>(entt)) {
          auto new_entity = temp_registry.create(entt);
          synchronized_entities.push_back(new_entity);

          // Copy synchronized components
          (copy_component_if_exists<SyncComponentsT>(entt, new_entity, temp_registry), ...);
        }
      }

      // Serialize the temporary registry
      entt::snapshot{temp_registry}.get<entity>(archive);
      (entt::snapshot{temp_registry}.template get<SyncComponentsT>(archive), ...);

      ostream.swap_vector(snapshot_buffer);

      // Update sync state
      state.last_sync = std::chrono::steady_clock::now();
      state.dirty_entities.clear();

    } catch (...) {
      // Return empty response on error
      snapshot_buffer.clear();
      synchronized_entities.clear();
    }

    co_return sync_response{.server_timestamp = std::chrono::steady_clock::now(), .snapshot_data = std::move(snapshot_buffer), .synchronized_entities = std::move(synchronized_entities)};
  }

  // RPC endpoint for adding components
  asio::awaitable<component_add_response> handle_component_add_request(component_add_request const& request) {
    try {
      // Validate entity exists or create if needed
      entity target = request.target_entity;
      if (!ecs_.valid(target)) {
        target = ecs_.create();
      }

      // Attempt to create the component
      bool success = component_registry_.create_component(request.component_type_name, ecs_, target, request.component_data);

      if (success) {
        // Mark entity as sync-enabled and client-owned
        ecs_.template emplace_if_not_exists<sync_enabled>(target);
        ecs_.template emplace_if_not_exists<client_owned>(target);

        // Mark as dirty for next sync
        auto& state = ecs_.template get<sync_state>();
        state.dirty_entities.insert(target);

        co_return component_add_response{.success = true, .error_message = "", .resulting_entity = target};
      } else {
        co_return component_add_response{.success = false, .error_message = "Failed to create component: " + request.component_type_name, .resulting_entity = entt_ext::null};
      }

    } catch (std::exception const& ex) {
      co_return component_add_response{.success = false, .error_message = std::string("Exception: ") + ex.what(), .resulting_entity = entt_ext::null};
    }
  }

  // Mark entity for synchronization
  void enable_sync(entity entt) {
    ecs_.template emplace_if_not_exists<sync_enabled>(entt);
    auto& state = ecs_.template get<sync_state>();
    state.dirty_entities.insert(entt);
  }

  // Remove entity from synchronization
  void disable_sync(entity entt) {
    ecs_.template remove<sync_enabled>(entt);
    auto& state = ecs_.template get<sync_state>();
    state.dirty_entities.erase(entt);
  }

private:
  template <typename ComponentT>
  void copy_component_if_exists(entity src_entity, entity dst_entity, entt::registry& dst_registry) {
    if (auto* component = ecs_.template try_get<ComponentT>(src_entity)) {
      dst_registry.emplace<ComponentT>(dst_entity, *component);
    }
  }

  std::vector<entity> get_sync_enabled_entities() {
    std::vector<entity> entities;
    for (auto [entt] : ecs_.template view<sync_enabled>().each()) {
      entities.push_back(entt);
    }
    return entities;
  }

  ecs&               ecs_;
  component_registry component_registry_;
};

// Client-side synchronization manager
template <typename... SyncComponentsT>
class sync_client {
public:
  explicit sync_client(ecs& ecs_instance)
    : ecs_(ecs_instance) {
    // Register all synchronizable components
    (component_registry_.template register_component<SyncComponentsT>(), ...);

    // Initialize sync state if not present
    if (!ecs_.template contains<sync_state>()) {
      ecs_.template get_or_emplace<sync_state>();
    }
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
      co_await ecs_.template merge_snapshot<cereal::PortableBinaryInputArchive, SyncComponentsT...>(archive);

      // Update sync state
      auto& state     = ecs_.template get<sync_state>();
      state.last_sync = response.server_timestamp;

      co_return true;

    } catch (...) {
      co_return false;
    }
  }

  // Create a component add request
  template <typename ComponentT>
  component_add_request create_component_request(entity target_entity, ComponentT const& component) {
    // Serialize the component
    grlx::rpc::buffer_type              component_data;
    grlx::rpc::ovectorstream            ostream;
    cereal::PortableBinaryOutputArchive archive(ostream);
    archive(component);
    ostream.swap_vector(component_data);

    return component_add_request{.target_entity = target_entity, .component_type_name = std::string(type_name<ComponentT>()), .component_data = std::move(component_data)};
  }

  // Get list of entities that need sync
  std::vector<entity> get_entities_of_interest() {
    std::vector<entity> entities;

    // Include all locally modified entities
    for (auto [entt] : ecs_.template view<sync_enabled>().each()) {
      entities.push_back(entt);
    }

    return entities;
  }

private:
  ecs&               ecs_;
  component_registry component_registry_;
};

} // namespace entt_ext::sync
