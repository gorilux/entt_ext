// Persistence / snapshot template implementations for entt_ext::ecs.
// Include this header instead of ecs.hpp when you need:
//   - ecs::load_snapshot<>()
//   - ecs::save_snapshot<>()
//   - ecs::merge_snapshot<>()
// Most consumers should include ecs.hpp directly (lighter).
#pragma once

#include "ecs.hpp"

#include <cereal/archives/portable_binary.hpp>
#include <cereal/cereal.hpp>
#include <entt/entity/snapshot.hpp>

namespace entt_ext {

// Snapshot functionality

template <typename ArchiveT, typename... ComponentsT>
asio::awaitable<void> ecs::load_snapshot(ArchiveT& ar) {

  co_await asio::async_compose<decltype(asio::use_awaitable), void()>(
      [&ar, this](auto&& self) {
        asio::post(main_io_context(), [self = std::move(self), &ar, this]() mutable {
          {
            emplace<loading_tag>(global_entity_);
            entt::snapshot_loader{registry_}.get<entt_ext::entity>(ar);

            // Load components - unwrap with_hierarchy<T> to get actual type T,
            // and also load parent<T>/children<T> if wrapped
            auto load_component = [&]<typename T>() {
              using ActualT = sync::unwrap_hierarchy_t<T>;
              entt::snapshot_loader{registry_}.template get<ActualT>(ar);
              if constexpr (sync::is_with_hierarchy_v<T>) {
                entt::snapshot_loader{registry_}.template get<entt_ext::parent<ActualT>>(ar);
                entt::snapshot_loader{registry_}.template get<entt_ext::children<ActualT>>(ar);
              }
            };
            (load_component.template operator()<ComponentsT>(), ...);

            remove<loading_tag>(global_entity_);
          }
          self.complete();
        });
      },
      asio::use_awaitable);

  co_return;
}

template <typename ArchiveT, typename... ComponentsT>
asio::awaitable<void> ecs::save_snapshot(ArchiveT& ar) const {
  co_return co_await asio::async_compose<decltype(asio::use_awaitable), void()>(
      [&ar, this](auto&& self) {
        asio::post(const_cast<asio::io_context&>(main_io_context()), [self = std::move(self), &ar, this]() mutable {
          {
            entt::snapshot{registry_}.get<entt_ext::entity>(ar);

            // Save components - unwrap with_hierarchy<T> to get actual type T,
            // and also save parent<T>/children<T> if wrapped
            auto save_component = [&]<typename T>() {
              using ActualT = sync::unwrap_hierarchy_t<T>;
              entt::snapshot{registry_}.template get<ActualT>(ar);
              if constexpr (sync::is_with_hierarchy_v<T>) {
                entt::snapshot{registry_}.template get<entt_ext::parent<ActualT>>(ar);
                entt::snapshot{registry_}.template get<entt_ext::children<ActualT>>(ar);
              }
            };
            (save_component.template operator()<ComponentsT>(), ...);
          }
          self.complete();
        });
      },
      asio::use_awaitable);
}

template <typename ArchiveT, typename... ComponentsT>
asio::awaitable<bool> ecs::merge_snapshot(ArchiveT& ar) {
  co_return co_await asio::async_compose<decltype(asio::use_awaitable), void(bool)>(
      [&ar, this](auto&& self) {
        asio::post(main_io_context(), [self = std::move(self), &ar, this]() mutable {
          {
            emplace<loading_tag>(global_entity_);

            // Load entities
            continuous_loader_.get<entt_ext::entity>(ar);

            // Load components - unwrap with_hierarchy<T> to get actual type T,
            // and also load parent<T>/children<T> if wrapped
            auto load_component = [&]<typename T>() {
              using ActualT = sync::unwrap_hierarchy_t<T>;
              continuous_loader_.template get<ActualT>(ar);
              if constexpr (sync::is_with_hierarchy_v<T>) {
                continuous_loader_.template get<entt_ext::parent<ActualT>>(ar);
                continuous_loader_.template get<entt_ext::children<ActualT>>(ar);
              }
            };
            (load_component.template operator()<ComponentsT>(), ...);

            continuous_loader_.orphans();

            // Remap entity references inside components after loading
            auto remap_component = [&]<typename T>() {
              using ActualT = sync::unwrap_hierarchy_t<T>;
              remap_component_entities<ActualT>();
              if constexpr (sync::is_with_hierarchy_v<T>) {
                remap_component_entities<entt_ext::parent<ActualT>>();
                remap_component_entities<entt_ext::children<ActualT>>();
              }
            };
            (remap_component.template operator()<ComponentsT>(), ...);

            // Diagnostic: log entity counts for each component type after merge.
            // Counts are obtained by iterating the view, not via view.size():
            // a single-component view over in_place_delete storage (e.g.
            // children<T>, which is index_set<entity, T>) has no size() — its
            // packed array carries tombstones. Iterating skips tombstones and
            // yields the true live count, and works for both storage policies.
            auto live_count = [](auto view) {
              std::size_t n = 0;
              for ([[maybe_unused]] auto entity : view) {
                ++n;
              }
              return n;
            };
            auto log_component = [this, &live_count]<typename T>() {
              using ActualT = sync::unwrap_hierarchy_t<T>;
              auto count    = live_count(registry_.template view<ActualT>());
              if (count > 0) {
                spdlog::info("[merge] {} entities with {}", count, entt::type_id<ActualT>().name());
              }
              if constexpr (sync::is_with_hierarchy_v<T>) {
                auto parent_count   = live_count(registry_.template view<entt_ext::parent<ActualT>>());
                auto children_count = live_count(registry_.template view<entt_ext::children<ActualT>>());
                if (parent_count > 0 || children_count > 0) {
                  spdlog::info("[merge]   parent<{}>: {}, children<{}>: {}",
                               entt::type_id<ActualT>().name(),
                               parent_count,
                               entt::type_id<ActualT>().name(),
                               children_count);
                }
              }
            };
            (log_component.template operator()<ComponentsT>(), ...);

            remove<loading_tag>(global_entity_);
          }
          self.complete(true);
        });
      },
      asio::use_awaitable);
}
} // namespace entt_ext
