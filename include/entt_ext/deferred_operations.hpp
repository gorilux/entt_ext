#pragma once

#include "ecs_fwd.hpp"
#include <entt/entity/fwd.hpp>
#include <functional>
#include <memory>

namespace entt_ext {

// Forward declare command_channel type (will be defined in ecs.hpp)
template <typename, typename>
class concurrent_channel_type;

// Alias that will match the actual definition in ecs.hpp
class command_channel;

// Deferred command that can be queued and executed later
struct deferred_command {
  enum class op_type {
    create,
    destroy,
    emplace,
    remove,
    emplace_if_not_exists,
    emplace_or_replace,
    patch,

    custom
  };

  op_type                                    operation;
  entt_ext::entity                           entity         = entt_ext::null;
  entt::id_type                              component_type = 0;
  std::function<asio::awaitable<void>(ecs&)> executor;

  // For create operations, store the created entity
  entt_ext::entity* result_entity = nullptr;
};

} // namespace entt_ext
