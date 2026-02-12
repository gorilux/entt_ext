#pragma once

#include <type_traits>

namespace entt_ext::sync {

// ============================================================================
// Hierarchy Component Wrapper - Opt-in hierarchy sync for specific components
// ============================================================================
//
// Usage:
//   sync_client<
//       with_hierarchy<Transform>,   // This gets parent<Transform>/children<Transform> synced
//       with_hierarchy<SceneNode>,   // This gets parent<SceneNode>/children<SceneNode> synced
//       Health,                      // This does NOT get hierarchy components
//       Velocity                     // This does NOT get hierarchy components
//   > client(ecs);
//
// ============================================================================

// Wrapper type to mark components that should have hierarchy sync
template <typename T>
struct with_hierarchy {
  using type = T;
};

// Type trait to detect with_hierarchy<T> wrapper
template <typename T>
struct is_with_hierarchy : std::false_type {};

template <typename T>
struct is_with_hierarchy<with_hierarchy<T>> : std::true_type {};

template <typename T>
inline constexpr bool is_with_hierarchy_v = is_with_hierarchy<T>::value;

// Unwrap with_hierarchy<T> to get T, or return T if not wrapped
template <typename T>
struct unwrap_hierarchy {
  using type = T;
};

template <typename T>
struct unwrap_hierarchy<with_hierarchy<T>> {
  using type = T;
};

template <typename T>
using unwrap_hierarchy_t = typename unwrap_hierarchy<T>::type;

} // namespace entt_ext::sync

