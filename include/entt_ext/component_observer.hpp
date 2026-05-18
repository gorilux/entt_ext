#pragma once

#include "ecs_fwd.hpp"
#include "index_map.hpp"
#include "type_name.hpp"

#include <memory>

namespace entt_ext {

using handler_id = size_t;

template <typename Type, typename EcsT>
class component_observer {

  // Internal handler types that don't take component parameter
  using internal_handler_type = std::function<void(EcsT&, entt_ext::entity)>;
  // Deferred async handlers receive a snapshot of the component captured
  // synchronously while the construct/destroy/update signal is firing — the
  // component still exists in storage and we are on the same non-suspended
  // call stack as the mutation, so the read is race-free. Re-fetching the
  // component from the deferred coroutine is unsafe: for on_destroy it is a
  // use-after-destroy, and under the concurrent ECS executor it is an
  // out-of-bounds data race on the component storage while another coroutine
  // reallocates it (e.g. mass entity destruction on reconnect).
  using internal_async_handler_type =
      std::function<asio::awaitable<void>(EcsT&, entt_ext::entity, std::shared_ptr<Type>)>;

public:
  component_observer(EcsT& ecs, entt_ext::entity entt) {
  }

  ~component_observer() = default;

  // std::string_view name() const {
  //   return type_name<Type>();
  // }

  template <typename FuncT>
  handler_id on_construct(FuncT&& func) {
    auto id = next_handler_id++;
    if constexpr (std::is_same_v<std::invoke_result_t<FuncT, EcsT&, entt_ext::entity, Type&>, asio::awaitable<void>>) {
      // Async handler - wrap it to handle component retrieval at registration time
      // Note: deferred async handlers may run after the entity/component is destroyed
      on_construct_async_handlers.emplace(id, [func = std::forward<FuncT>(func)](EcsT& ecs, entt_ext::entity entt, std::shared_ptr<Type> snapshot) -> asio::awaitable<void> {
        if (snapshot) {
          co_await func(ecs, entt, *snapshot);
        } else {
          Type t{};
          co_await func(ecs, entt, t);
        }
      });
    } else {
      // Sync handler - wrap it to handle component retrieval at registration time
      on_construct_handlers.emplace(id, [func = std::forward<FuncT>(func)](EcsT& ecs, entt_ext::entity entt) {
        if constexpr (entt::component_traits<Type>::page_size > 0u) {
          func(ecs, entt, ecs.template get<Type>(entt));
        } else {
          Type t;
          func(ecs, entt, t);
        }
      });
    }
    return id;
  }

  template <typename FuncT>
  handler_id on_destroy(FuncT&& func) {
    auto id = next_handler_id++;

    // Special case for entt::entity type - it's not a component, so handlers take only 2 params
    if constexpr (std::is_same_v<Type, entt::entity>) {
      if constexpr (std::is_same_v<std::invoke_result_t<FuncT, EcsT&, entt_ext::entity>, asio::awaitable<void>>) {
        // Async handler for entity destruction
        on_destroy_async_handlers.emplace(id, [func = std::forward<FuncT>(func)](EcsT& ecs, entt_ext::entity entt, std::shared_ptr<Type>) -> asio::awaitable<void> {
          co_await func(ecs, entt);
        });
      } else {
        // Sync handler for entity destruction
        on_destroy_handlers.emplace(id, [func = std::forward<FuncT>(func)](EcsT& ecs, entt_ext::entity entt) {
          func(ecs, entt);
        });
      }
    } else if constexpr (std::is_same_v<std::invoke_result_t<FuncT, EcsT&, entt_ext::entity, Type&>, asio::awaitable<void>>) {
      // Async handler - wrap it to handle component retrieval at registration time
      // Note: deferred async handlers may run after the entity is destroyed/recycled
      on_destroy_async_handlers.emplace(id, [func = std::forward<FuncT>(func)](EcsT& ecs, entt_ext::entity entt, std::shared_ptr<Type> snapshot) -> asio::awaitable<void> {
        if (snapshot) {
          co_await func(ecs, entt, *snapshot);
        } else {
          Type t{};
          co_await func(ecs, entt, t);
        }
      });
    } else {
      // Sync handler - wrap it to handle component retrieval at registration time
      on_destroy_handlers.emplace(id, [func = std::forward<FuncT>(func)](EcsT& ecs, entt_ext::entity entt) {
        if constexpr (entt::component_traits<Type>::page_size > 0u) {
          if (ecs.template all_of<Type>(entt)) {
            func(ecs, entt, ecs.template get<Type>(entt));
          } else {
            Type t{};
            func(ecs, entt, t);
          }
        } else {
          Type t{};
          func(ecs, entt, t);
        }
      });
    }
    return id;
  }

  template <typename FuncT>
  handler_id on_update(FuncT&& func) {
    auto id = next_handler_id++;
    if constexpr (std::is_same_v<std::invoke_result_t<FuncT, EcsT&, entt_ext::entity, Type&>, asio::awaitable<void>>) {
      // Async handler - wrap it to handle component retrieval at registration time
      // Note: deferred async handlers may run after the entity/component is destroyed
      on_update_async_handlers.emplace(id, [func = std::forward<FuncT>(func)](EcsT& ecs, entt_ext::entity entt, std::shared_ptr<Type> snapshot) -> asio::awaitable<void> {
        if (snapshot) {
          co_await func(ecs, entt, *snapshot);
        } else {
          Type t{};
          co_await func(ecs, entt, t);
        }
      });
    } else {
      // Sync handler - wrap it to handle component retrieval at registration time
      on_update_handlers.emplace(id, [func = std::forward<FuncT>(func)](EcsT& ecs, entt_ext::entity entt) {
        if constexpr (entt::component_traits<Type>::page_size > 0u) {
          func(ecs, entt, ecs.template get<Type>(entt));
        } else {
          Type t;
          func(ecs, entt, t);
        }
      });
    }
    return id;
  }

  bool on_construct_disconnect(handler_id id) {
    if (auto it = on_construct_handlers.find(id); it != on_construct_handlers.end()) {
      on_construct_handlers.erase(it->first);
      return true;
    }
    if (auto it = on_construct_async_handlers.find(id); it != on_construct_async_handlers.end()) {
      on_construct_async_handlers.erase(it->first);
      return true;
    }
    return false;
  }

  bool on_destroy_disconnect(handler_id id) {
    if (auto it = on_destroy_handlers.find(id); it != on_destroy_handlers.end()) {
      on_destroy_handlers.erase(it->first);
      return true;
    }
    if (auto it = on_destroy_async_handlers.find(id); it != on_destroy_async_handlers.end()) {
      on_destroy_async_handlers.erase(it->first);
      return true;
    }
    return false;
  }

  bool on_update_disconnect(handler_id id) {
    if (auto it = on_update_handlers.find(id); it != on_update_handlers.end()) {
      on_update_handlers.erase(it->first);
      return true;
    }
    if (auto it = on_update_async_handlers.find(id); it != on_update_async_handlers.end()) {
      on_update_async_handlers.erase(it->first);
      return true;
    }
    return false;
  }

  // Capture the component value synchronously, on the same non-suspended
  // call stack as the construct/destroy/update signal, while it is still in
  // storage. The returned snapshot is what deferred async handlers receive;
  // they must never re-read the storage themselves.
  //
  // Move-only components (e.g. resource handles) cannot be snapshotted; for
  // those this returns null and the deferred handler is a no-op. Such types
  // are not used with async observers, but component_observer<T> is still
  // instantiated for every emplaced component, so this must compile for them.
  std::shared_ptr<Type> snapshot_component(EcsT& ecs, entt_ext::entity entt) {
    if constexpr (!std::is_same_v<Type, entt::entity> && entt::component_traits<Type>::page_size > 0u &&
                  std::is_copy_constructible_v<Type>) {
      if (ecs.valid(entt) && ecs.template all_of<Type>(entt)) {
        return std::make_shared<Type>(ecs.template get<Type>(entt));
      }
    }
    return nullptr;
  }

  void dispatch_on_construct(EcsT& ecs, entt_ext::entity entt) {
    // spdlog::debug("on_construct: {}:{}", static_cast<size_t>(entt), name());

    // Sync handlers - call directly
    for (auto& [id, handler] : on_construct_handlers) {
      handler(ecs, entt);
    }

    if (on_construct_async_handlers.empty()) {
      return;
    }

    auto snapshot = snapshot_component(ecs, entt);
    for (auto& [id, handler] : on_construct_async_handlers) {
      ecs.defer_async([handler, entt, snapshot](EcsT& ecs_ref) -> asio::awaitable<void> {
        co_await handler(ecs_ref, entt, snapshot);
      });
    }
  }

  void dispatch_on_destroy(EcsT& ecs, entt_ext::entity entt) {
    // spdlog::debug("on_destroy: {}:{}", static_cast<size_t>(entt), name());

    // Sync handlers - call directly (component still present in storage here)
    for (auto& [id, handler] : on_destroy_handlers) {
      handler(ecs, entt);
    }

    if (on_destroy_async_handlers.empty()) {
      return;
    }

    // Snapshot the component NOW (race-free, see snapshot_component). The
    // deferred coroutines must NOT re-read the storage later — by then the
    // component is gone and, under the concurrent ECS executor, the storage
    // may be mid-reallocation (e.g. mass entity destruction on reconnect),
    // which is what caused the out-of-bounds crash in element_at.
    auto snapshot = snapshot_component(ecs, entt);
    for (auto& [id, handler] : on_destroy_async_handlers) {
      ecs.defer_async([handler, entt, snapshot](EcsT& ecs_ref) -> asio::awaitable<void> {
        co_await handler(ecs_ref, entt, snapshot);
      });
    }
  }

  void dispatch_on_update(EcsT& ecs, entt_ext::entity entt) {
    // spdlog::debug("on_update: {}:{}", static_cast<size_t>(entt), name());

    // Sync handlers - call directly
    for (auto& [id, handler] : on_update_handlers) {
      handler(ecs, entt);
    }

    if (on_update_async_handlers.empty()) {
      return;
    }

    auto snapshot = snapshot_component(ecs, entt);
    for (auto& [id, handler] : on_update_async_handlers) {
      ecs.defer_async([handler, entt, snapshot](EcsT& ecs_ref) -> asio::awaitable<void> {
        co_await handler(ecs_ref, entt, snapshot);
      });
    }
  }

private:
  handler_id                                         next_handler_id = 0;
  index_map<handler_id, internal_handler_type>       on_construct_handlers;
  index_map<handler_id, internal_handler_type>       on_destroy_handlers;
  index_map<handler_id, internal_handler_type>       on_update_handlers;
  index_map<handler_id, internal_async_handler_type> on_construct_async_handlers;
  index_map<handler_id, internal_async_handler_type> on_destroy_async_handlers;
  index_map<handler_id, internal_async_handler_type> on_update_async_handlers;
};

} // namespace entt_ext