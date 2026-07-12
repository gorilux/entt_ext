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

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>

#include <spdlog/spdlog.h>

#include <exception>
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

      // Suppress sync_client's "send to server" observers while the
      // registry rehydrates — the emplace events here are local restore
      // events, not user-driven mutations, and the session may not even
      // exist yet.
      client_.push_suppress_observer_rpcs();
      struct guard {
        SyncClient& c;
        ~guard() { c.pop_suppress_observer_rpcs(); }
      } _suppress{client_};

      // The cache file is a server-keyed snapshot, byte-identical to a
      // live sync_response. Replay it through the sync_client's own
      // continuous_loader so the restored entities are remapped and
      // tracked exactly as if the server had just pushed them — a later
      // server snapshot then reuses them instead of duplicating. This is
      // also why we don't use entt::snapshot_loader: EnTT 4.0 asserts an
      // empty registry, which entt_ext never has (global entity).
      //
      // restore_cached_snapshot is a coroutine (it shares the live
      // remap path). load_now() is intentionally synchronous — called
      // from the module constructor before ecs::run() — so drive it to
      // completion on a transient io_context. The restore touches only
      // the registry/loader (no network), so run() returns immediately.
      boost::asio::io_context drive_ctx;
      std::exception_ptr      restore_error;
      boost::asio::co_spawn(
          drive_ctx,
          [this, &archive]() -> boost::asio::awaitable<void> {
            co_await client_.restore_cached_snapshot(archive);
          },
          [&restore_error](std::exception_ptr ep) { restore_error = ep; });
      drive_ctx.run();
      if (restore_error) {
        std::rethrow_exception(restore_error);
      }

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

      // Persist a server-keyed snapshot (the sync_client translates the
      // live local entity IDs to their server IDs). The file is therefore
      // independent of the local IDs this run happened to assign and is
      // restored through the same continuous_loader path as a live server
      // snapshot — see sync_client::save_cached_snapshot.
      client_.save_cached_snapshot(archive);
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

    // pending_deletes lives on the global entity (see
    // entt_ext/sync/pending_changes.hpp) and isn't part of ComponentsT, so
    // it needs its own dirty-trigger wiring to be captured by the
    // periodic save.
    auto& pending_deletes_obs = ecs_.template component_observer<pending_deletes>();
    pending_deletes_obs.on_construct(trigger);
    pending_deletes_obs.on_update(trigger);
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
