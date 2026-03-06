#pragma once

#include "ecs.hpp"
#include "type_name.hpp"

#include <grlx/rpc/encoder.hpp>
#include <grlx/rpc/message.hpp>

#include <cereal/archives/portable_binary.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/unordered_map.hpp>
#include <cereal/types/unordered_set.hpp>
#include <cereal/types/vector.hpp>

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <sstream>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace entt_ext::sync {

namespace asio = boost::asio;

using version_type = std::chrono::steady_clock::time_point;

// ============================================================================
// Sync Component List Helper - Ensures client/server use same component order
// ============================================================================

// Helper to define a synchronized component list with compile-time ordering
template <typename... Components>
struct sync_component_list {
  using types = std::tuple<Components...>;

  // Get number of components at compile time
  static constexpr size_t size() {
    return sizeof...(Components);
  }

private:
  // Deterministic hash for cross-platform consistency.
  // std::hash is implementation-defined and differs between libstdc++ and libc++,
  // so we use FNV-1a which produces identical results on all platforms.
  static constexpr uint64_t fnv1a(std::string_view sv) {
    uint64_t hash = 14695981039346656037ULL; // FNV offset basis
    for (char c : sv) {
      hash ^= static_cast<uint64_t>(static_cast<unsigned char>(c));
      hash *= 1099511628211ULL; // FNV prime
    }
    return hash;
  }

  // Helper to add component name to hash and output stream
  template <typename T>
  static void add_component_to_protocol(std::ostringstream& oss, size_t& hash) {
    // Use the full type name (including with_hierarchy wrapper if present)
    // This ensures client/server with different hierarchy configs don't match
    auto name = type_name<T>();
    hash ^= fnv1a(name) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    oss << name << "_";
  }

public:
  // Generate a protocol version string based on component types and their order
  static std::string generate_protocol_version() {
    std::ostringstream oss;
    size_t             hash = size();

    // Include component names for debugging
    oss << "sync_v1_";

    // Combine type names in order to create a version identifier
    (add_component_to_protocol<Components>(oss, hash), ...);

    oss << "hash_" << std::hex << hash;

    return oss.str();
  }

  // Compile-time type identity check
  template <typename... OtherComponents>
  static constexpr bool matches() {
    return std::is_same_v<std::tuple<Components...>, std::tuple<OtherComponents...>>;
  }
};

// Macro for easy definition of sync component lists
#define ENTT_EXT_SYNC_COMPONENTS(...) entt_ext::sync::sync_component_list<__VA_ARGS__>

// Helper to extract component types from sync_component_list
template <typename SyncList>
struct sync_list_traits;

template <typename... Components>
struct sync_list_traits<sync_component_list<Components...>> {
  using component_tuple = std::tuple<Components...>;

  template <template <typename...> class Template>
  using apply = Template<Components...>;
};

// Note: with_hierarchy<T>, is_with_hierarchy_v<T>, and unwrap_hierarchy_t<T>
// are defined in hierarchy_wrapper.hpp (included via ecs.hpp)

// Type trait to detect hierarchy components (parent<T> or children<T>)
template <typename T>
struct is_hierarchy_component : std::false_type {};

template <typename T>
struct is_hierarchy_component<entt_ext::parent<T>> : std::true_type {};

template <typename T>
struct is_hierarchy_component<entt_ext::children<T>> : std::true_type {};

template <typename T>
inline constexpr bool is_hierarchy_component_v = is_hierarchy_component<T>::value;

// Backward compatibility macro
#define ENTT_EXT_SYNC_COMPONENTS_WITH_HIERARCHY(...) entt_ext::sync::sync_component_list<__VA_ARGS__>

// ============================================================================
// Handshake message types for session management
struct handshake_request {
  std::string client_name;      // Optional client identifier
  std::string client_version;   // Optional client version info
  std::string protocol_version; // Protocol version based on component types and order

  template <typename Archive>
  void serialize(Archive& archive) {
    archive(client_name, client_version, protocol_version);
  }
};

struct handshake_response {
  bool                                  success;
  std::string                           session_id;       // Unique session identifier
  std::string                           error_message;    // Error details if handshake failed
  std::string                           protocol_version; // Server's protocol version
  std::chrono::steady_clock::time_point server_timestamp;

  template <typename Archive>
  void serialize(Archive& archive) {
    archive(success, session_id, error_message, protocol_version, server_timestamp);
  }
};

// Synchronization message types
struct sync_request {
  std::string                           session_id; // Session ID from handshake
  std::chrono::steady_clock::time_point client_timestamp;
  std::vector<entity>                   entities_of_interest; // Client can specify which entities to sync

  template <typename Archive>
  void serialize(Archive& archive) {
    archive(session_id, client_timestamp, entities_of_interest);
  }
};

struct sync_response {
  std::chrono::steady_clock::time_point server_timestamp;
  grlx::rpc::buffer_type                snapshot_data;

  template <typename Archive>
  void serialize(Archive& archive) {
    archive(server_timestamp, snapshot_data);
  }
};

template <typename ComponentT>
struct component_remove_request {
  std::string  session_id; // Session ID from handshake
  version_type sync_version;
  entity       target_entity;
  template <typename Archive>
  void serialize(Archive& archive) {
    archive(session_id, sync_version, target_entity);
  }
};

template <typename ComponentT>
struct component_remove_response {
  bool         success;
  version_type sync_version;
  std::string  error_message;

  template <typename Archive>
  void serialize(Archive& archive) {
    archive(success, sync_version, error_message);
  }
};

// Component-specific request/response types (type-safe, no serialization overhead)
template <typename ComponentT>
struct component_update_request {
  std::string  session_id; // Session ID from handshake
  version_type sync_version;
  entity       target_entity;
  ComponentT   component_data;

  template <typename Archive>
  void serialize(Archive& archive) {
    archive(session_id, sync_version, target_entity, component_data);
  }
};

template <typename ComponentT>
struct component_update_response {
  bool        success;
  std::string error_message;

  template <typename Archive>
  void serialize(Archive& archive) {
    archive(success, error_message);
  }
};

template <typename ComponentT>
struct local_component_changed {
  version_type sync_version;
};

template <typename ComponentT>
struct sync_component_state {
  version_type sync_version;
};

template <typename ComponentT>
struct local_component_removed {
  version_type sync_version;
};

// Tag components for sync management
struct server_authority {};  // Mark components that only server can modify
struct dirty_client {};      // Mark entities that have been modified locally and need to be pushed
struct merge_in_progress {}; // Mark entities that are being merged from the server

// Sync state tracking
struct sync_state {
  std::chrono::steady_clock::time_point last_sync;
  std::chrono::steady_clock::time_point last_push;      // Last time client pushed changes
  std::unordered_set<entity>            dirty_entities; // Server-side dirty entities
  std::unordered_set<entity>            client_dirty;   // Client-side entities that need to be pushed
  std::unordered_set<std::string>       synchronized_component_types;

  template <typename Archive>
  void serialize(Archive& archive) {
    archive(last_sync, last_push, dirty_entities, client_dirty, synchronized_component_types);
  }
};

// Entity creation request - clients use this to request server entities
struct entity_create_request {
  std::string session_id;    // Session ID from handshake
  entity      client_entity; // Client's local entity ID

  template <typename Archive>
  void serialize(Archive& archive) {
    archive(session_id, client_entity);
  }
};

struct entity_create_response {
  bool        success;
  entity      server_entity; // The server entity that was created
  std::string error_message;

  template <typename Archive>
  void serialize(Archive& archive) {
    archive(success, server_entity, error_message);
  }
};

// Entity mapping sync messages
struct entity_mapping_update_request {
  std::string                        session_id;               // Session ID from handshake
  std::unordered_map<entity, entity> server_to_client_mapping; // Server entity -> Client entity

  template <typename Archive>
  void serialize(Archive& archive) {
    archive(session_id, server_to_client_mapping);
  }
};

struct entity_mapping_update_response {
  bool        success;
  std::string error_message;

  template <typename Archive>
  void serialize(Archive& archive) {
    archive(success, error_message);
  }
};

// Entity destruction request - clients notify server when entities are destroyed
struct entity_destroy_request {
  std::string  session_id;    // Session ID from handshake
  entity       server_entity; // The server entity to destroy
  version_type sync_version;

  template <typename Archive>
  void serialize(Archive& archive) {
    archive(session_id, server_entity, sync_version);
  }
};

struct entity_destroy_response {
  bool        success;
  std::string error_message;

  template <typename Archive>
  void serialize(Archive& archive) {
    archive(success, error_message);
  }
};

} // namespace entt_ext::sync