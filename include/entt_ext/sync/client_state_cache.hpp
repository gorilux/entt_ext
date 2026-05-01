#pragma once

// Phase 2 of the offline-first plan (see docs/offline_first.md).
//
// A reusable companion module for a sync_client that persists the synced
// registry — entities + components + the continuous_loader entity-id
// mapping — to a single binary file. On startup, load_now() restores the
// cached state synchronously before the host calls ecs::run(), so the
// GUI has data to show even when the server isn't reachable.
//
// Limitations of phase 2 (closed in later phases):
//   - Mutations made while disconnected are NOT propagated to the server
//     on reconnect; the next snapshot from the server overwrites them.
//   - No conflict resolution: server is the source of truth. If you
//     opened a workout offline and the server has a different state for
//     the same plan when you reconnect, the offline session is lost.
//
// Phase 3 adds pending-change tracking; phase 4 adds the reconcile RPC
// + per-component conflict policies.

#include <entt_ext/ecs.hpp>
#include <entt_ext/hierarchy_wrapper.hpp>
#include <entt_ext/sync_common.hpp>
#include <entt_ext/sync/pending_changes.hpp>

#include <cereal/archives/portable_binary.hpp>
#include <entt/entity/snapshot.hpp>

#include <spdlog/spdlog.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

namespace entt_ext::sync {

namespace detail {
// Stamped on the global entity by the auto-save observers; consumed by
// the periodic save system. Local to this header to avoid polluting the
// app's component namespace.
struct cache_dirty {};
} // namespace detail

template <typename SyncClient, typename... ComponentsT>
class client_state_cache {
public:
  struct config {
    std::string filename;
    // Periodic auto-save cadence (seconds). The save itself is gated on
    // a `cache_dirty` flag set by component observers, so a quiescent
    // app does no disk I/O.
    double auto_save_interval = 5.0;
  };

  client_state_cache(ecs& ecs_ref, SyncClient& client, config cfg)
      : ecs_(ecs_ref)
      , client_(client)
      , filename_(std::move(cfg.filename)) {
    setup_auto_save_observers();
    register_save_system(cfg.auto_save_interval);
  }

  ~client_state_cache() = default;

  client_state_cache(client_state_cache const&)            = delete;
  client_state_cache& operator=(client_state_cache const&) = delete;

  // Synchronous restore. Call before ecs::run() so the connect system
  // sees a populated registry on the very first tick. No-op if the file
  // doesn't exist yet (first run on a fresh client).
  void load_now() {
    std::error_code ec;
    if (!std::filesystem::exists(filename_, ec) || ec) {
      return;
    }

    std::ifstream ifs(filename_, std::ios::binary);
    if (!ifs) {
      spdlog::warn("[sync cache] failed to open {} for read", filename_);
      return;
    }

    try {
      cereal::PortableBinaryInputArchive archive(ifs);

      auto& reg = ecs_.registry();

      // Suppress sync_client's "send to server" observers while the
      // registry rehydrates — the emplace events here are local restore
      // events, not user-driven mutations, and the session may not even
      // exist yet.
      client_.push_suppress_observer_rpcs();
      struct guard {
        SyncClient& c;
        ~guard() { c.pop_suppress_observer_rpcs(); }
      } _suppress{client_};

      // Entities first — must be loaded before components so component
      // storages have somewhere to put their values.
      entt::snapshot_loader{reg}.template get<entt_ext::entity>(archive);

      // Each synced component, plus parent<T>/children<T> when wrapped,
      // plus the pending_create<T>/pending_update<T> markers stamped by
      // sync_client's observers (phase 3). Order matters and must match
      // save_now() exactly.
      auto load_component = [&]<typename T>() {
        using ActualT = unwrap_hierarchy_t<T>;
        entt::snapshot_loader{reg}.template get<ActualT>(archive);
        if constexpr (is_with_hierarchy_v<T>) {
          entt::snapshot_loader{reg}.template get<entt_ext::parent<ActualT>>(archive);
          entt::snapshot_loader{reg}.template get<entt_ext::children<ActualT>>(archive);
        }
        entt::snapshot_loader{reg}.template get<pending_create<ActualT>>(archive);
        entt::snapshot_loader{reg}.template get<pending_update<ActualT>>(archive);
      };
      (load_component.template operator()<ComponentsT>(), ...);

      // Mapping last — entity validity check inside load_mapping needs
      // the registry to already be repopulated.
      client_.load_mapping(archive);

      spdlog::info("[sync cache] restored from {}", filename_);
    } catch (std::exception const& ex) {
      spdlog::warn("[sync cache] failed to load {}: {} — renaming to .bak and continuing fresh",
                   filename_, ex.what());
      std::error_code rename_ec;
      std::filesystem::rename(filename_, filename_ + ".bak", rename_ec);
    }
  }

  // Force-save now (for shutdown handlers etc). Normal saves go through
  // the periodic system gated on `cache_dirty`.
  void save_now() {
    auto tmp = filename_ + ".tmp";

    {
      std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
      if (!ofs) {
        spdlog::warn("[sync cache] failed to open {} for write", tmp);
        return;
      }

      cereal::PortableBinaryOutputArchive archive(ofs);
      auto&                               reg = ecs_.registry();

      entt::snapshot{reg}.template get<entt_ext::entity>(archive);

      auto save_component = [&]<typename T>() {
        using ActualT = unwrap_hierarchy_t<T>;
        entt::snapshot{reg}.template get<ActualT>(archive);
        if constexpr (is_with_hierarchy_v<T>) {
          entt::snapshot{reg}.template get<entt_ext::parent<ActualT>>(archive);
          entt::snapshot{reg}.template get<entt_ext::children<ActualT>>(archive);
        }
        entt::snapshot{reg}.template get<pending_create<ActualT>>(archive);
        entt::snapshot{reg}.template get<pending_update<ActualT>>(archive);
      };
      (save_component.template operator()<ComponentsT>(), ...);

      client_.save_mapping(archive);
    }

    // Atomic rename so a crash mid-write can't leave a half-written
    // cache file that breaks the next startup.
    std::error_code ec;
    std::filesystem::rename(tmp, filename_, ec);
    if (ec) {
      spdlog::warn("[sync cache] rename {} → {} failed: {}", tmp, filename_, ec.message());
    }
  }

private:
  void setup_auto_save_observers() {
    auto trigger = [this](ecs& e, entity, auto&) {
      e.template emplace_if_not_exists<detail::cache_dirty>(e.get_global_entity());
    };

    auto attach = [this, &trigger]<typename T>() {
      using ActualT = unwrap_hierarchy_t<T>;
      auto& obs     = ecs_.template component_observer<ActualT>();
      obs.on_construct(trigger);
      obs.on_update(trigger);
      obs.on_destroy(trigger);

      // Hierarchy components (parent<T>/children<T>) also count as
      // dirty — saving them is what makes the cache survive a child
      // being added under a parent etc.
      if constexpr (is_with_hierarchy_v<T>) {
        auto& parent_obs = ecs_.template component_observer<entt_ext::parent<ActualT>>();
        parent_obs.on_construct(trigger);
        parent_obs.on_update(trigger);
        parent_obs.on_destroy(trigger);

        auto& children_obs = ecs_.template component_observer<entt_ext::children<ActualT>>();
        children_obs.on_construct(trigger);
        children_obs.on_update(trigger);
        children_obs.on_destroy(trigger);
      }
    };
    (attach.template operator()<ComponentsT>(), ...);
  }

  void register_save_system(double interval) {
    ecs_.template system<detail::cache_dirty>()
        .each([this](ecs& e, system&, double, entity ent) {
          save_now();
          e.template remove<detail::cache_dirty>(ent);
        })
        .interval(interval)
        .stage(stage::post_render);
  }

  ecs&        ecs_;
  SyncClient& client_;
  std::string filename_;
};

} // namespace entt_ext::sync
