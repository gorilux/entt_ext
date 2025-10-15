#pragma once

// ============================================================================
// ENTT_EXT SYNC USAGE EXAMPLE
// ============================================================================
// This file demonstrates how to use the sync component list feature to ensure
// that client and server use the same component types in the same order.
//
// IMPORTANT: Component order MUST match between client and server for
// serialization to work correctly!
// ============================================================================

#include "sync_client.hpp"
#include "sync_common.hpp"
#include "sync_server.hpp"

// Example: Define your component types
struct Position {
  float x, y, z;

  template <typename Archive>
  void serialize(Archive& archive) {
    archive(x, y, z);
  }
};

struct Velocity {
  float vx, vy, vz;

  template <typename Archive>
  void serialize(Archive& archive) {
    archive(vx, vy, vz);
  }
};

struct Health {
  int current, max;

  template <typename Archive>
  void serialize(Archive& archive) {
    archive(current, max);
  }
};

// ============================================================================
// METHOD 1: Define a shared component list (RECOMMENDED)
// ============================================================================
// Define this ONCE in a shared header that both client and server include.
// This guarantees compile-time ordering consistency.

using GameSyncComponents = entt_ext::sync::sync_component_list<Position, // Order matters!
                                                               Velocity, // These must be in the same order
                                                               Health    // on both client and server
                                                               >;

// ============================================================================
// METHOD 2: Use the convenience macro
// ============================================================================
// Alternative syntax using the provided macro:

using GameSyncComponentsMacro = ENTT_EXT_SYNC_COMPONENTS(Position, Velocity, Health);

// ============================================================================
// USAGE IN YOUR CODE
// ============================================================================

namespace example {

// Server-side usage
class GameServer {
public:
  explicit GameServer(entt_ext::ecs& ecs)
    : sync_server_(ecs) {
    // The sync_server will automatically:
    // 1. Generate a protocol version string from the component list
    // 2. Validate incoming client connections have matching protocol
    // 3. Reject clients with mismatched component lists
  }

  boost::asio::awaitable<void> start(uint16_t port) {
    co_await sync_server_.start(port);
  }

private:
  // Option A: Use the sync_component_list directly
  entt_ext::sync::sync_server<Position, Velocity, Health> sync_server_;

  // Option B: Extract types from the list using apply (more maintainable)
  // entt_ext::sync::sync_list_traits<GameSyncComponents>::template apply<entt_ext::sync::sync_server> sync_server_;
};

// Client-side usage
class GameClient {
public:
  explicit GameClient(entt_ext::ecs& ecs)
    : sync_client_(ecs) {
    // The sync_client will automatically:
    // 1. Generate the same protocol version string (if components match)
    // 2. Send it during handshake
    // 3. Validate server's protocol matches
    // 4. Fail connection if protocol doesn't match
  }

  boost::asio::awaitable<bool> connect(std::string const& host, uint16_t port) {
    co_return co_await sync_client_.connect(host, port, "GameClient", "1.0");
  }

private:
  // CRITICAL: Component order MUST match the server!
  entt_ext::sync::sync_client<Position, Velocity, Health> sync_client_;
};

// ============================================================================
// COMPILE-TIME SAFETY CHECK
// ============================================================================
// You can add compile-time checks to ensure component lists match:

template <typename ClientList, typename ServerList>
constexpr bool validate_sync_lists() {
  static_assert(ClientList::size() == ServerList::size(), "Client and Server component lists have different sizes!");

  static_assert(std::is_same_v<typename ClientList::types, typename ServerList::types>, "Client and Server component lists don't match!");

  return true;
}

// Example usage:
// static_assert(validate_sync_lists<GameSyncComponents, GameSyncComponents>());

// ============================================================================
// WHAT HAPPENS AT RUNTIME
// ============================================================================
//
// When the client connects:
// 1. Client generates protocol version: "sync_v1_Position_Velocity_Health_hash_abc123"
// 2. Client sends handshake with protocol version
// 3. Server validates protocol version matches its own
// 4. If match: Connection succeeds
//    If mismatch: Connection fails with clear error message
//
// Benefits:
// - Automatic detection of component order mismatches
// - Clear error messages when protocol doesn't match
// - No silent data corruption
// - Easy debugging with human-readable protocol strings
//
// ============================================================================
// BEST PRACTICES
// ============================================================================
//
// 1. Define component list ONCE in a shared header
// 2. Use type aliases (using X = ...) instead of repeating the list
// 3. Add static_assert checks to catch mistakes at compile time
// 4. Never change component order without updating version
// 5. Log protocol versions for debugging
//
// Example of a well-organized structure:
//
// // shared_sync_config.hpp (included by both client and server)
// using MySyncComponents = ENTT_EXT_SYNC_COMPONENTS(
//     Position,
//     Velocity,
//     Health
// );
//
// // server.hpp
// #include "shared_sync_config.hpp"
// using MyServer = sync_list_traits<MySyncComponents>::template apply<sync_server>;
//
// // client.hpp
// #include "shared_sync_config.hpp"
// using MyClient = sync_list_traits<MySyncComponents>::template apply<sync_client>;
//
// ============================================================================

} // namespace example
