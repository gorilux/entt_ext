#pragma once

// Out-of-line implementations of sync_client_with_channel<...> member
// functions. Auto-included from sync_client.hpp unless the build system
// defines ENTT_EXT_SYNC_CLIENT_NO_IMPL — in which case this header should
// be included only from a single per-app instantiation TU that performs
// `template class sync_client_with_channel<...>` for the app's pack.

#include <entt_ext/sync_client.hpp>

#include <entt/entity/snapshot.hpp>

#include <cereal/types/string.hpp>
#include <cereal/types/unordered_map.hpp>
#include <cereal/types/unordered_set.hpp>
#include <cereal/types/vector.hpp>

#include <boost/asio/use_awaitable.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <stdexcept>

namespace entt_ext::sync {

// ============================================================================
// Constructor
// ============================================================================

template <typename ChannelT, typename... SyncComponentsT>
template <typename... ChannelArgs>
sync_client_with_channel<ChannelT, SyncComponentsT...>::sync_client_with_channel(ecs& ecs_instance, ChannelArgs&&... channel_args)
  : ecs_(ecs_instance)
  , protocol_version_(sync_component_list<SyncComponentsT...>::generate_protocol_version())
  //, protocol_version_("sync_v1_")
  , continuous_loader_(ecs_.registry())
  , rpc_client_(std::forward<ChannelArgs>(channel_args)...)
  , entity_create_strand_(asio::make_strand(ecs_.concurrent_io_context().get_executor())) {

  // Initialize sync state if not present
  if (!ecs_.template contains<sync_state>()) {
    ecs_.template get_or_emplace<sync_state>();
  }
  // Set up automatic synchronization for each component type
  (setup_automatic_sync<SyncComponentsT>(), ...);
  setup_entity_sync();

  // spdlog::info("Sync client initialized with protocol version: {}", protocol_version_);
}

// ============================================================================
// Connection / handshake / snapshot
// ============================================================================

template <typename ChannelT, typename... SyncComponentsT>
asio::awaitable<bool> sync_client_with_channel<ChannelT, SyncComponentsT...>::connect(std::string const& host,
                                                                                      std::uint16_t      port,
                                                                                      std::string const& client_name,
                                                                                      std::string const& client_version,
                                                                                      std::string const& username,
                                                                                      std::string const& password) {
  try {
    spdlog::info("sync_client::connect: connecting to {}:{} as '{}' (client='{}' v{})", host, port, username, client_name, client_version);

    // First establish TCP connection
    auto          executor = co_await asio::this_coro::executor;
    tcp::resolver resolver(executor);
    auto          endpoints = co_await resolver.async_resolve(host, std::to_string(port), asio::use_awaitable);
    co_await rpc_client_.connect(*endpoints.begin());
    spdlog::info("sync_client::connect: TCP/TLS channel established to {}:{}", host, port);

    // Set up notification handlers for real-time sync.
    //
    // This is intentionally done BEFORE the handshake, not after it succeeds.
    // Registration is a purely local map insert (no network), it is idempotent
    // across reconnects, and each handler self-guards on session_id_ so it stays
    // inert until the handshake assigns one. Doing it first closes a drop window:
    // the instant the server accepts the handshake it may start pushing component
    // updates / entity events (and request_snapshot fires right after), and the
    // dispatch path silently drops any frame whose handler isn't registered yet.
    // On handshake failure the disconnect() below calls clear_notification_handlers(),
    // so there is no leak from registering early.
    setup_notification_handlers();

    // Perform handshake to get session ID (includes authentication)
    spdlog::info("sync_client::connect: starting handshake");
    bool handshake_success = co_await perform_handshake(client_name, client_version, username, password);
    if (!handshake_success) {
      spdlog::warn("sync_client::connect: handshake failed (auth rejected or no session); disconnecting");
      // Disconnect on handshake failure
      co_await disconnect();
      co_return false;
    }
    spdlog::info("sync_client::connect: handshake ok, session_id='{}'", session_id_);

    // Phase 4 of the offline-first plan (see docs/offline_first.md):
    // before pulling the server's snapshot, push every pending_create<T>
    // / pending_update<T> stamped on local entities while we were
    // disconnected. This way the server's snapshot — which arrives
    // next — already reflects our offline edits, and the merge in
    // continuous_loader doesn't roll them back. Failures are kept on
    // the entity (the marker stays) so the next reconnect retries.
    co_await reconcile_pending_changes();
    spdlog::info("sync_client::connect: reconciled pending offline changes");

    co_await request_snapshot();
    // NB: request_snapshot only *queues* the snapshot load (defer_awaitable to the
    // main-thread channel); the actual ingest + mapping happens later and is
    // marked by "snapshot fully loaded from server". Don't read this as "applied".
    spdlog::info("sync_client::connect: snapshot received; load queued on main-thread channel; connect returning true");

    co_return true;
  } catch (std::exception const& ex) {
    spdlog::error("sync_client::connect: failed for {}:{} — {}", host, port, ex.what());
    session_id_.clear();
    throw; // Let the caller see the actual error
  } catch (...) {
    spdlog::error("sync_client::connect: failed for {}:{} — unknown exception", host, port);
    session_id_.clear();
    throw; // Let the caller see the actual error
  }
}

template <typename ChannelT, typename... SyncComponentsT>
asio::awaitable<bool> sync_client_with_channel<ChannelT, SyncComponentsT...>::keepalive() {
  if (!rpc_client_.is_connected() || session_id_.empty()) {
    co_return false;
  }
  try {
    sync_keepalive_request request{.session_id = session_id_};
    auto                   response = co_await rpc_client_.template invoke<sync_keepalive_response>("sync_keepalive", std::move(request));
    if (!response.success) {
      spdlog::warn("Sync keepalive rejected: {}", response.error_message);
      co_return false;
    }
    co_return true;
  } catch (std::exception const& ex) {
    spdlog::warn("Sync keepalive failed: {}", ex.what());
    co_return false;
  } catch (...) {
    co_return false;
  }
}

template <typename ChannelT, typename... SyncComponentsT>
asio::awaitable<bool> sync_client_with_channel<ChannelT, SyncComponentsT...>::request_snapshot(std::vector<entity> const& entities_of_interest) {
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

template <typename ChannelT, typename... SyncComponentsT>
asio::awaitable<bool> sync_client_with_channel<ChannelT, SyncComponentsT...>::connect_and_sync(std::string const&         host,
                                                                                               std::uint16_t              port,
                                                                                               std::vector<entity> const& entities_of_interest,
                                                                                               std::string const&         client_name,
                                                                                               std::string const&         client_version) {
  bool connected = co_await connect(host, port, client_name, client_version);
  if (!connected) {
    co_return false;
  }

  bool synced = co_await request_snapshot(entities_of_interest);
  co_return synced;
}

template <typename ChannelT, typename... SyncComponentsT>
asio::awaitable<bool> sync_client_with_channel<ChannelT, SyncComponentsT...>::apply_sync_response(sync_response const& response) {
  try {
    if (response.snapshot_data.empty()) {
      co_return false;
    }

    ecs_.defer_awaitable([this, response](entt_ext::ecs& ecs) -> asio::awaitable<void> {
      // Load the snapshot into our ECS
      // Use continuous loader to merge entities without conflicts
      // Note: The snapshot contains server entity IDs that get mapped to client entity IDs
      spdlog::info("sync_client: loading snapshot from server (deferred body now running on main thread)");
      loading_snapshot_ = true;
      // Suppress async observer queueing during the bulk load. The
      // writeable on_construct/on_update bodies already bail on
      // loading_snapshot_, but each emplace would still queue a defer that
      // saturates command_channel_ for large snapshots. The flag stays set
      // until the matching clear below runs on the same channel.
      ecs.set_async_observers_muted(true);

      // The decode + ingest below can throw (short/corrupt snapshot, cereal
      // PortableBinaryInputArchive underflow, a bad entity in the stream). This
      // runs as its OWN coroutine on the command channel — it is NOT covered by
      // apply_sync_response's outer try/catch. An escaping exception here would
      // be swallowed by the channel with no log AND leave loading_snapshot_
      // stuck true + observers muted, silently killing all future sync. So
      // catch it here: log it loudly and restore sync state before bailing.
      try {
        grlx::rpc::ibufferstream           istream(&response.snapshot_data[0], response.snapshot_data.size());
        cereal::PortableBinaryInputArchive archive(istream);

        // Single shared snapshot-ingest path (also used by the offline-first
        // restore_cached_snapshot): entities → components → orphans → remap.
        co_await load_snapshot_from_archive(archive);

        // Stamp last-sync here, on the command-channel (main) thread. Doing it
        // on apply_sync_response's own (connect-path / POOL) coroutine raced the
        // main loop's registry access — a concurrent ecs_.get<sync_state>() +
        // write while the main thread mutates the registry.
        ecs.template get<sync_state>().last_sync = response.server_timestamp;
      } catch (std::exception const& ex) {
        spdlog::error("sync_client: snapshot load FAILED (decode/ingest threw): {} — restoring sync state", ex.what());
        loading_snapshot_ = false;
        ecs.set_async_observers_muted(false);
        co_return;
      } catch (...) {
        spdlog::error("sync_client: snapshot load FAILED (unknown exception during decode/ingest) — restoring sync state");
        loading_snapshot_ = false;
        ecs.set_async_observers_muted(false);
        co_return;
      }

      ecs_.defer_awaitable([this](entt_ext::ecs& ecs_inner) -> asio::awaitable<void> {
        loading_snapshot_ = false;
        ecs_inner.set_async_observers_muted(false);
        spdlog::info("sync_client: snapshot fully loaded from server (mappings established, observers unmuted)");
        co_return;
      });

      co_return;
    });

    co_return true;

  } catch (std::exception const& ex) {
    // This outer catch only covers the SYNCHRONOUS prefix — the empty check and
    // the defer_awaitable *enqueue* call. The decode/ingest runs in the deferred
    // body and has its own try/catch above. Reaching here means we failed to
    // even queue the load (e.g. the command channel rejected it). Previously
    // this swallowed the exception with no log, hiding the failure entirely.
    spdlog::error("sync_client::apply_sync_response: failed to queue snapshot load: {}", ex.what());
    loading_snapshot_ = false;
    co_return false;
  } catch (...) {
    spdlog::error("sync_client::apply_sync_response: failed to queue snapshot load (unknown exception)");
    loading_snapshot_ = false;
    co_return false;
  }
}

// ============================================================================
// Snapshot ingest (shared by live sync and offline-first cache restore)
// ============================================================================

template <typename ChannelT, typename... SyncComponentsT>
asio::awaitable<void>
sync_client_with_channel<ChannelT, SyncComponentsT...>::load_snapshot_from_archive(cereal::PortableBinaryInputArchive& archive) {
  // Entity table first (server IDs → fresh local IDs, recorded in the
  // continuous_loader's remote↔local maps), then every sync component
  // (and hierarchy) into the mapped locals, then orphan cleanup, then
  // remap any entity references the components carry.
  continuous_loader_.get<entt_ext::entity>(archive);
  (load_component_and_hierarchy<SyncComponentsT>(archive), ...);
  continuous_loader_.orphans();
  (co_await remap_component_and_hierarchy<SyncComponentsT>(), ...);
  co_return;
}

// ============================================================================
// Offline-first registry cache (see docs/offline_first.md)
// ============================================================================

template <typename ChannelT, typename... SyncComponentsT>
template <typename ComponentT>
void sync_client_with_channel<ChannelT, SyncComponentsT...>::copy_component_to_server_keyed(entt::registry&            tmp,
                                                                                            std::vector<entity> const& mapped_local,
                                                                                            std::vector<entity> const& mapped_server) {
  using ActualT = unwrap_hierarchy_t<ComponentT>;

  for (std::size_t i = 0; i < mapped_local.size(); ++i) {
    auto loc = mapped_local[i];
    auto srv = mapped_server[i];
    if (auto* c = ecs_.template try_get<ActualT>(loc)) {
      ActualT value = *c;
      // Translate any entity references the component carries from local
      // to server IDs — same hook the live send path uses.
      if constexpr (requires(ActualT& x, continuous_loader_with_mapping<entt::registry> const& l) { x.map_entities_to_remote(l); }) {
        value.map_entities_to_remote(continuous_loader_);
      }
      tmp.template emplace<ActualT>(srv, std::move(value));
    }
  }

  if constexpr (is_with_hierarchy_v<ComponentT>) {
    for (std::size_t i = 0; i < mapped_local.size(); ++i) {
      auto loc = mapped_local[i];
      auto srv = mapped_server[i];

      if (auto* p = ecs_.template try_get<entt_ext::parent<ActualT>>(loc)) {
        auto parent_srv = continuous_loader_.to_remote(p->entity);
        if (parent_srv != entt_ext::null) {
          entt_ext::parent<ActualT> np = *p;
          np.entity                    = parent_srv;
          tmp.template emplace<entt_ext::parent<ActualT>>(srv, np);
        }
      }

      if (auto* ch = ecs_.template try_get<entt_ext::children<ActualT>>(loc)) {
        entt_ext::children<ActualT> filtered;
        for (auto child : *ch) {
          auto child_srv = continuous_loader_.to_remote(child);
          if (child_srv != entt_ext::null) {
            filtered.insert(child_srv);
          }
        }
        if (!filtered.empty()) {
          tmp.template emplace<entt_ext::children<ActualT>>(srv, std::move(filtered));
        }
      }
    }
  }
}

template <typename ChannelT, typename... SyncComponentsT>
void sync_client_with_channel<ChannelT, SyncComponentsT...>::save_cached_snapshot(cereal::PortableBinaryOutputArchive& archive) {
  // Gather every synced local entity that already has a server mapping,
  // paired with its server ID. Offline-only entities (no server ID yet)
  // are intentionally not cached — that is the documented phase-2
  // limitation; they will be handled by phase-3 pending-change tracking.
  std::vector<entity>        mapped_local;
  std::vector<entity>        mapped_server;
  std::unordered_set<entity> seen;

  auto collect = [&]<typename T>() {
    using ActualT = unwrap_hierarchy_t<T>;
    for (auto e : ecs_.view<ActualT>()) {
      if (seen.contains(e)) {
        continue;
      }
      auto srv = continuous_loader_.to_remote(e);
      if (srv != entt_ext::null) {
        seen.insert(e);
        mapped_local.push_back(e);
        mapped_server.push_back(srv);
      }
    }
  };
  (collect.template operator()<SyncComponentsT>(), ...);

  // Build a server-keyed temporary registry (mirror of the server's
  // build_filtered_registry): the entity table on disk holds server IDs,
  // so the cache survives across runs that assign different local IDs.
  entt::registry tmp;
  for (auto srv : mapped_server) {
    [[maybe_unused]] auto created = tmp.create(srv);
  }
  (copy_component_to_server_keyed<SyncComponentsT>(tmp, mapped_local, mapped_server), ...);

  entt::snapshot{tmp}.get<entt_ext::entity>(archive);
  auto save_one = [&]<typename T>() {
    using ActualT = unwrap_hierarchy_t<T>;
    entt::snapshot{tmp}.template get<ActualT>(archive);
    if constexpr (is_with_hierarchy_v<T>) {
      entt::snapshot{tmp}.template get<entt_ext::parent<ActualT>>(archive);
      entt::snapshot{tmp}.template get<entt_ext::children<ActualT>>(archive);
    }
  };
  (save_one.template operator()<SyncComponentsT>(), ...);

  // Pending markers for the already-mapped entities above (phase 4
  // follow-up — see copy_pending_markers_to_server_keyed).
  auto save_pending = [&]<typename T>() {
    using ActualT = unwrap_hierarchy_t<T>;
    copy_pending_markers_to_server_keyed<T>(tmp, mapped_local, mapped_server);
    entt::snapshot{tmp}.template get<pending_create<ActualT>>(archive);
    entt::snapshot{tmp}.template get<pending_update<ActualT>>(archive);
  };
  (save_pending.template operator()<SyncComponentsT>(), ...);

  // Offline-only entities: same criterion as `seen` above, inverted — any
  // entity carrying a SyncComponentsT component that never made it into
  // `seen` has no server mapping at all (see save_offline_component).
  std::vector<entity>                       offline_local;
  std::unordered_map<entity, std::uint32_t> local_to_temp;
  auto                                      collect_offline = [&]<typename T>() {
    using ActualT = unwrap_hierarchy_t<T>;
    for (auto e : ecs_.view<ActualT>()) {
      if (seen.contains(e) || local_to_temp.contains(e)) {
        continue;
      }
      local_to_temp.emplace(e, static_cast<std::uint32_t>(offline_local.size()));
      offline_local.push_back(e);
    }
  };
  (collect_offline.template operator()<SyncComponentsT>(), ...);

  archive(static_cast<std::uint64_t>(offline_local.size()));
  (save_offline_component<SyncComponentsT>(archive, offline_local, local_to_temp), ...);

  // Tombstones for deletes made while offline (or whose entity_destroy RPC
  // failed) — see entt_ext/sync/pending_changes.hpp.
  archive(ecs_.template get_or_emplace<pending_deletes>(ecs_.get_global_entity()).entries);
}

template <typename ChannelT, typename... SyncComponentsT>
asio::awaitable<void> sync_client_with_channel<ChannelT, SyncComponentsT...>::restore_cached_snapshot(cereal::PortableBinaryInputArchive& archive) {
  // Identical ingest path to a live server snapshot — the cache file is
  // shaped exactly like sync_response.snapshot_data.
  co_await load_snapshot_from_archive(archive);

  // Pending markers for the already-mapped entities just restored above
  // (must run after load_snapshot_from_archive — see load_pending_markers).
  (load_pending_markers<SyncComponentsT>(archive), ...);

  // Offline-only entities: brand-new local entities, no continuous_loader
  // mapping (see save_offline_component).
  std::uint64_t offline_count = 0;
  archive(offline_count);
  std::vector<entity> temp_to_local;
  temp_to_local.reserve(offline_count);
  for (std::uint64_t i = 0; i < offline_count; ++i) {
    temp_to_local.push_back(ecs_.create());
  }
  (load_offline_component<SyncComponentsT>(archive, temp_to_local), ...);

  // Tombstones for deletes made while offline.
  std::vector<pending_deletes::entry> tombstones;
  archive(tombstones);
  if (!tombstones.empty()) {
    ecs_.template get_or_emplace<pending_deletes>(ecs_.get_global_entity()).entries = std::move(tombstones);
  }

  co_return;
}

// ============================================================================
// Offline-first cache: pending markers on already-mapped entities
// ============================================================================

template <typename ChannelT, typename... SyncComponentsT>
template <typename ComponentT>
void sync_client_with_channel<ChannelT, SyncComponentsT...>::copy_pending_markers_to_server_keyed(
    entt::registry& tmp, std::vector<entity> const& mapped_local, std::vector<entity> const& mapped_server) {
  using ActualT = unwrap_hierarchy_t<ComponentT>;

  for (std::size_t i = 0; i < mapped_local.size(); ++i) {
    auto loc = mapped_local[i];
    auto srv = mapped_server[i];
    if (ecs_.template all_of<pending_create<ActualT>>(loc)) {
      // pending_create<T> is an empty marker (see pending_changes.hpp) —
      // entt's try_get<T> would need std::addressof(cpool->get(entt)), which
      // is ill-formed for empty types since get() returns void for them.
      // all_of<T> only needs presence, and there is no value to copy anyway.
      tmp.template emplace<pending_create<ActualT>>(srv);
    }
    if (auto* pu = ecs_.template try_get<pending_update<ActualT>>(loc)) {
      tmp.template emplace<pending_update<ActualT>>(srv, *pu);
    }
  }
}

template <typename ChannelT, typename... SyncComponentsT>
template <typename ComponentT>
void sync_client_with_channel<ChannelT, SyncComponentsT...>::load_pending_markers(cereal::PortableBinaryInputArchive& archive) {
  using ActualT = unwrap_hierarchy_t<ComponentT>;
  continuous_loader_.template get<pending_create<ActualT>>(archive);
  continuous_loader_.template get<pending_update<ActualT>>(archive);
}

// ============================================================================
// Offline-first cache: offline-only entities (no server mapping at all)
// ============================================================================

template <typename ChannelT, typename... SyncComponentsT>
void sync_client_with_channel<ChannelT, SyncComponentsT...>::write_offline_ref(
    cereal::PortableBinaryOutputArchive& archive, entity target, std::unordered_map<entity, std::uint32_t> const& local_to_temp) {
  if (target == entt_ext::null) {
    archive(offline_ref_kind::none);
    return;
  }
  if (auto it = local_to_temp.find(target); it != local_to_temp.end()) {
    archive(offline_ref_kind::offline_temp);
    archive(it->second);
    return;
  }
  auto srv = continuous_loader_.to_remote(target);
  if (srv != entt_ext::null) {
    archive(offline_ref_kind::server);
    archive(srv);
    return;
  }
  // Neither an offline-only sibling nor a server-mapped entity (destroyed,
  // or belongs to a type outside SyncComponentsT) — drop the reference
  // rather than persist a dangling id.
  archive(offline_ref_kind::none);
}

template <typename ChannelT, typename... SyncComponentsT>
entt_ext::entity sync_client_with_channel<ChannelT, SyncComponentsT...>::read_offline_ref(cereal::PortableBinaryInputArchive& archive,
                                                                                          std::vector<entity> const& temp_to_local) {
  offline_ref_kind kind{};
  archive(kind);
  switch (kind) {
    case offline_ref_kind::server: {
      entity srv{entt_ext::null};
      archive(srv);
      return continuous_loader_.to_local(srv);
    }
    case offline_ref_kind::offline_temp: {
      std::uint32_t idx = 0;
      archive(idx);
      return idx < temp_to_local.size() ? temp_to_local[idx] : entt_ext::null;
    }
    default:
      return entt_ext::null;
  }
}

template <typename ChannelT, typename... SyncComponentsT>
template <typename ComponentT>
void sync_client_with_channel<ChannelT, SyncComponentsT...>::save_offline_component(
    cereal::PortableBinaryOutputArchive& archive, std::vector<entity> const& offline_local,
    std::unordered_map<entity, std::uint32_t> const& local_to_temp) {
  using ActualT = unwrap_hierarchy_t<ComponentT>;

  // Component value.
  std::vector<entity> present;
  for (auto e : offline_local) {
    if (ecs_.template try_get<ActualT>(e)) {
      present.push_back(e);
    }
  }
  archive(static_cast<std::uint64_t>(present.size()));
  for (auto e : present) {
    archive(local_to_temp.at(e));
    archive(*ecs_.template try_get<ActualT>(e));
  }

  // pending_create marker.
  std::vector<entity> creating;
  for (auto e : offline_local) {
    // all_of<T>, not try_get<T> — pending_create<T> is an empty marker (see
    // pending_changes.hpp) and entt's try_get<T> is ill-formed for empty
    // types (get() returns void, so std::addressof(...) can't compile).
    if (ecs_.template all_of<pending_create<ActualT>>(e)) {
      creating.push_back(e);
    }
  }
  archive(static_cast<std::uint64_t>(creating.size()));
  for (auto e : creating) {
    archive(local_to_temp.at(e));
  }

  // pending_update marker.
  std::vector<entity> updating;
  for (auto e : offline_local) {
    if (ecs_.template try_get<pending_update<ActualT>>(e)) {
      updating.push_back(e);
    }
  }
  archive(static_cast<std::uint64_t>(updating.size()));
  for (auto e : updating) {
    archive(local_to_temp.at(e));
    archive(*ecs_.template try_get<pending_update<ActualT>>(e));
  }

  if constexpr (is_with_hierarchy_v<ComponentT>) {
    std::vector<entity> with_parent;
    for (auto e : offline_local) {
      if (ecs_.template try_get<entt_ext::parent<ActualT>>(e)) {
        with_parent.push_back(e);
      }
    }
    archive(static_cast<std::uint64_t>(with_parent.size()));
    for (auto e : with_parent) {
      archive(local_to_temp.at(e));
      write_offline_ref(archive, ecs_.template try_get<entt_ext::parent<ActualT>>(e)->entity, local_to_temp);
    }

    std::vector<entity> with_children;
    for (auto e : offline_local) {
      if (ecs_.template try_get<entt_ext::children<ActualT>>(e)) {
        with_children.push_back(e);
      }
    }
    archive(static_cast<std::uint64_t>(with_children.size()));
    for (auto e : with_children) {
      archive(local_to_temp.at(e));
      auto* ch = ecs_.template try_get<entt_ext::children<ActualT>>(e);
      archive(static_cast<std::uint64_t>(ch->size()));
      for (auto child : *ch) {
        write_offline_ref(archive, child, local_to_temp);
      }
    }
  }
}

template <typename ChannelT, typename... SyncComponentsT>
template <typename ComponentT>
void sync_client_with_channel<ChannelT, SyncComponentsT...>::load_offline_component(cereal::PortableBinaryInputArchive& archive,
                                                                                    std::vector<entity> const&          temp_to_local) {
  using ActualT = unwrap_hierarchy_t<ComponentT>;

  std::uint64_t present_count = 0;
  archive(present_count);
  for (std::uint64_t i = 0; i < present_count; ++i) {
    std::uint32_t idx = 0;
    archive(idx);
    ActualT value{};
    archive(value);
    if (idx < temp_to_local.size()) {
      ecs_.template emplace<ActualT>(temp_to_local[idx], std::move(value));
    }
  }

  std::uint64_t creating_count = 0;
  archive(creating_count);
  for (std::uint64_t i = 0; i < creating_count; ++i) {
    std::uint32_t idx = 0;
    archive(idx);
    if (idx < temp_to_local.size()) {
      ecs_.template emplace<pending_create<ActualT>>(temp_to_local[idx]);
    }
  }

  std::uint64_t updating_count = 0;
  archive(updating_count);
  for (std::uint64_t i = 0; i < updating_count; ++i) {
    std::uint32_t idx = 0;
    archive(idx);
    pending_update<ActualT> value{};
    archive(value);
    if (idx < temp_to_local.size()) {
      ecs_.template emplace<pending_update<ActualT>>(temp_to_local[idx], value);
    }
  }

  if constexpr (is_with_hierarchy_v<ComponentT>) {
    std::uint64_t parent_count = 0;
    archive(parent_count);
    for (std::uint64_t i = 0; i < parent_count; ++i) {
      std::uint32_t idx = 0;
      archive(idx);
      entity resolved = read_offline_ref(archive, temp_to_local);
      if (idx < temp_to_local.size() && resolved != entt_ext::null) {
        ecs_.template emplace<entt_ext::parent<ActualT>>(temp_to_local[idx], entt_ext::parent<ActualT>{resolved});
      }
    }

    std::uint64_t children_count = 0;
    archive(children_count);
    for (std::uint64_t i = 0; i < children_count; ++i) {
      std::uint32_t idx = 0;
      archive(idx);
      std::uint64_t child_count = 0;
      archive(child_count);
      entt_ext::children<ActualT> set;
      for (std::uint64_t c = 0; c < child_count; ++c) {
        entity resolved = read_offline_ref(archive, temp_to_local);
        if (resolved != entt_ext::null) {
          set.insert(resolved);
        }
      }
      if (idx < temp_to_local.size() && !set.empty()) {
        ecs_.template emplace<entt_ext::children<ActualT>>(temp_to_local[idx], std::move(set));
      }
    }
  }
}

// ============================================================================
// Reconcile pending changes
// ============================================================================

template <typename ChannelT, typename... SyncComponentsT>
asio::awaitable<void> sync_client_with_channel<ChannelT, SyncComponentsT...>::reconcile_pending_changes() {
  // Creates: iterate until no entity changed marker state.
  for (int pass = 0; pass < 16; ++pass) {
    bool progress = false;
    co_await reconcile_creates_helper<SyncComponentsT...>(progress);
    if (!progress)
      break;
  }

  // Updates: order independent — every entity here is already mapped
  // (it was emplaced from a server snapshot at some prior point), so
  // a single pass is enough.
  co_await reconcile_updates_helper<SyncComponentsT...>();

  // Deletes last, per the offline-first design doc's ordering (creates,
  // then updates, then deletes).
  co_await reconcile_pending_deletes();

  co_return;
}

template <typename ChannelT, typename... SyncComponentsT>
asio::awaitable<void> sync_client_with_channel<ChannelT, SyncComponentsT...>::reconcile_pending_deletes() {
  // Same threading rule as reconcile_creates_for/reconcile_updates_for:
  // this runs on the concurrent_io_context, so registry access is hopped
  // to main.
  auto pending = co_await ecs_.invoke_on_main([](entt_ext::ecs& ecs) -> std::vector<pending_deletes::entry> {
    if (auto* pd = ecs.template try_get<pending_deletes>(ecs.get_global_entity())) {
      return pd->entries;
    }
    return {};
  });

  std::vector<entity> confirmed;
  for (auto const& tomb : pending) {
    try {
      entity_destroy_request request{.session_id = session_id_, .server_entity = tomb.server_entity, .sync_version = std::chrono::steady_clock::now()};
      auto response = co_await rpc_client_.template invoke<entity_destroy_response>("entity_destroy", std::move(request));
      if (response.success) {
        confirmed.push_back(tomb.server_entity);
      } else {
        spdlog::warn("[reconcile] delete failed for server entity {}: {}", static_cast<int>(tomb.server_entity), response.error_message);
      }
    } catch (std::exception const& ex) {
      spdlog::warn("[reconcile] delete failed for server entity {}: {}", static_cast<int>(tomb.server_entity), ex.what());
    } catch (...) {
      spdlog::warn("[reconcile] delete failed for server entity {}: unknown", static_cast<int>(tomb.server_entity));
    }
  }

  if (!confirmed.empty()) {
    co_await ecs_.invoke_on_main([confirmed = std::move(confirmed)](entt_ext::ecs& ecs) {
      if (auto* pd = ecs.template try_get<pending_deletes>(ecs.get_global_entity())) {
        auto& entries = pd->entries;
        entries.erase(std::remove_if(entries.begin(),
                                     entries.end(),
                                     [&](auto const& e) {
                                       return std::find(confirmed.begin(), confirmed.end(), e.server_entity) != confirmed.end();
                                     }),
                     entries.end());
      }
      return 0;
    });
  }

  co_return;
}

template <typename ChannelT, typename... SyncComponentsT>
template <typename First, typename... Rest>
asio::awaitable<void> sync_client_with_channel<ChannelT, SyncComponentsT...>::reconcile_creates_helper(bool& progress) {
  co_await reconcile_creates_for<First>(progress);
  if constexpr (sizeof...(Rest) > 0) {
    co_await reconcile_creates_helper<Rest...>(progress);
  }
  co_return;
}

template <typename ChannelT, typename... SyncComponentsT>
template <typename First, typename... Rest>
asio::awaitable<void> sync_client_with_channel<ChannelT, SyncComponentsT...>::reconcile_updates_helper() {
  co_await reconcile_updates_for<First>();
  if constexpr (sizeof...(Rest) > 0) {
    co_await reconcile_updates_helper<Rest...>();
  }
  co_return;
}

template <typename ChannelT, typename... SyncComponentsT>
template <typename ComponentT>
asio::awaitable<void> sync_client_with_channel<ChannelT, SyncComponentsT...>::reconcile_creates_for(bool& progress) {
  using ActualT            = unwrap_hierarchy_t<ComponentT>;
  constexpr bool read_only = is_server_only_v<ComponentT>;

  if constexpr (read_only) {
    // server_only<T> components are never written by the client.
    co_return;
  } else {
    // Registry access (view / try_get / remove) MUST run on the main thread.
    // This reconcile runs inside the connect coroutine, which executes on the
    // concurrent_io_context; touching the registry here races the main loop's
    // systems (e.g. the startup `deserialize` snapshot load) over entt's
    // component-pool map. TSan flags it as a data race on
    // dense_map<type_id, sparse_set> (concurrent assure/find), and on Android
    // NDK/scudo the corrupted map surfaces as a heap-corruption crash-loop
    // (reportInvalidChunkState) the instant the daemon auto-connects. So:
    // snapshot the pending entities + their component values on the main thread,
    // do the network I/O off-thread, then remove the markers back on main.
    struct create_batch {
      std::vector<std::pair<entity, ActualT>> items;
      bool                                    removed_stale = false;
    };
    auto batch = co_await ecs_.invoke_on_main([](entt_ext::ecs& ecs) {
      create_batch b;
      for (auto e : ecs.template view<pending_create<ActualT>>()) {
        if (auto* component = ecs.template try_get<ActualT>(e)) {
          b.items.emplace_back(e, *component);
        } else {
          // Component was removed locally between observer and reconcile;
          // drop the now-stale marker.
          ecs.template remove<pending_create<ActualT>>(e);
          b.removed_stale = true;
        }
      }
      return b;
    });

    std::vector<entity> sent;
    for (auto& [e, component] : batch.items) {
      try {
        co_await send_component_to_server<ActualT>(e, component, std::chrono::steady_clock::now());
        sent.push_back(e);
      } catch (std::exception const& ex) {
        spdlog::warn("[reconcile] create failed for {} entity {}: {}", type_name<ActualT>(), static_cast<int>(e), ex.what());
      } catch (...) {
        spdlog::warn("[reconcile] create failed for {} entity {}: unknown", type_name<ActualT>(), static_cast<int>(e));
      }
    }

    if (!sent.empty()) {
      co_await ecs_.invoke_on_main([sent = std::move(sent)](entt_ext::ecs& ecs) {
        for (auto e : sent) {
          if (ecs.valid(e)) {
            ecs.template remove<pending_create<ActualT>>(e);
          }
        }
        return 0;
      });
      progress = true;
    }
    if (batch.removed_stale) {
      progress = true;
    }
  }
  co_return;
}

template <typename ChannelT, typename... SyncComponentsT>
template <typename ComponentT>
asio::awaitable<void> sync_client_with_channel<ChannelT, SyncComponentsT...>::reconcile_updates_for() {
  using ActualT            = unwrap_hierarchy_t<ComponentT>;
  constexpr bool read_only = is_server_only_v<ComponentT>;

  if constexpr (read_only) {
    co_return;
  } else {
    // Same threading rule as reconcile_creates_for: iterate/mutate the registry
    // only on the main thread (this runs on the concurrent_io_context and would
    // otherwise race the main loop over entt's component-pool map). Snapshot on
    // main, send off-thread, remove markers back on main.
    auto items = co_await ecs_.invoke_on_main([](entt_ext::ecs& ecs) {
      std::vector<std::pair<entity, ActualT>> out;
      for (auto e : ecs.template view<pending_update<ActualT>>()) {
        if (auto* component = ecs.template try_get<ActualT>(e)) {
          out.emplace_back(e, *component);
        } else {
          ecs.template remove<pending_update<ActualT>>(e);
        }
      }
      return out;
    });

    std::vector<entity> sent;
    for (auto& [e, component] : items) {
      try {
        co_await send_component_to_server<ActualT>(e, component, std::chrono::steady_clock::now());
        sent.push_back(e);
      } catch (std::exception const& ex) {
        spdlog::warn("[reconcile] update failed for {} entity {}: {}", type_name<ActualT>(), static_cast<int>(e), ex.what());
      } catch (...) {
        spdlog::warn("[reconcile] update failed for {} entity {}: unknown", type_name<ActualT>(), static_cast<int>(e));
      }
    }

    if (!sent.empty()) {
      co_await ecs_.invoke_on_main([sent = std::move(sent)](entt_ext::ecs& ecs) {
        for (auto e : sent) {
          if (ecs.valid(e)) {
            ecs.template remove<pending_update<ActualT>>(e);
          }
        }
        return 0;
      });
    }
  }
  co_return;
}

// ============================================================================
// Notification handlers
// ============================================================================

template <typename ChannelT, typename... SyncComponentsT>
void sync_client_with_channel<ChannelT, SyncComponentsT...>::setup_notification_handlers() {
  // Register handlers for component-specific notifications
  (setup_component_notification_handlers<SyncComponentsT>(), ...);

  // Handle entity destruction notifications from server.
  //
  // Empty session_id means "broadcast to everyone" — the server uses that
  // for server-initiated changes (e.g. notify_entity_destruction_to_all_clients).
  // A non-empty session_id means the notification is addressed to a specific
  // recipient (e.g. the "_to_other_clients" routing path) and we should
  // only process it if it's ours.
  rpc_client_.register_notification_handler("entity_destroyed", [this](entity_destroy_request const& request) {
    if (!request.session_id.empty() && request.session_id != session_id_)
      return;

    ecs_.defer([this, request](entt_ext::ecs& ecs) {
      entity server_entity = request.server_entity;
      auto   client_entity = continuous_loader_.to_local(server_entity);

      if (client_entity == entt_ext::null) {
        spdlog::debug("Received entity destruction for unmapped server entity {}", static_cast<int>(server_entity));
        return;
      }

      if (ecs.valid(client_entity)) {
        spdlog::debug("Server requested entity destruction: server={} client={}", static_cast<int>(server_entity), static_cast<int>(client_entity));
        // Remove mapping before destroying so the on_destroy observer
        // won't send a redundant notification back to the server
        continuous_loader_.remove_mapping_by_remote(server_entity);
        ecs.destroy(client_entity);
      }
    });
  });
}

template <typename ChannelT, typename... SyncComponentsT>
template <typename ComponentT>
void sync_client_with_channel<ChannelT, SyncComponentsT...>::setup_component_notification_handlers() {
  using ActualT = unwrap_hierarchy_t<ComponentT>;

  // Set up for the component itself
  setup_component_notification_handlers_impl<ActualT>();

  // Also set up for hierarchy components if wrapped with with_hierarchy<T>
  if constexpr (is_with_hierarchy_v<ComponentT>) {
    setup_component_notification_handlers_impl<entt_ext::parent<ActualT>>();
    setup_component_notification_handlers_impl<entt_ext::children<ActualT>>();
  }
}

template <typename ChannelT, typename... SyncComponentsT>
template <typename ComponentT>
void sync_client_with_channel<ChannelT, SyncComponentsT...>::setup_component_notification_handlers_impl() {

  std::string component_name = std::string(type_name<ComponentT>());

  // Handle component updates. Empty session_id == broadcast to everyone;
  // see entity_destroyed handler above for the full rationale.
  std::string update_notification = "component_updated_" + component_name;
  rpc_client_.register_notification_handler(update_notification, [this](component_update_request<ComponentT> const& request) {
    if (!request.session_id.empty() && request.session_id != session_id_)
      return;

    // Defer ECS modifications to avoid concurrent registry access
    // (notification handlers run from the RPC receive path)
    ecs_.defer([this, request](entt_ext::ecs& ecs) {
      entity server_entity = request.target_entity;
      auto   client_entity = continuous_loader_.to_local(server_entity);

      if (client_entity == entt_ext::null) {
        spdlog::debug("Received component update for unmapped server entity {} ({})", static_cast<int>(server_entity), type_name<ComponentT>());
        client_entity = ecs.create();
        continuous_loader_.insert_mapping(server_entity, client_entity);
        spdlog::debug("Created new client entity {} for server entity {}", static_cast<int>(client_entity), static_cast<int>(server_entity));
      }

      if (ecs.valid(client_entity)) {
        ecs.template emplace_or_replace<component_update_request<ComponentT>>(client_entity, request);
      }
    });
  });

  // Handle component removals. Empty session_id == broadcast to everyone;
  // see entity_destroyed handler above for the full rationale.
  std::string remove_notification = "component_removed_" + component_name;
  rpc_client_.register_notification_handler(remove_notification, [this](component_remove_request<ComponentT> const& request) {
    if (!request.session_id.empty() && request.session_id != session_id_)
      return;

    // Defer ECS modifications to avoid concurrent registry access
    ecs_.defer([this, request](entt_ext::ecs& ecs) {
      entity server_entity = request.target_entity;
      auto   client_entity = continuous_loader_.to_local(server_entity);

      if (client_entity == entt_ext::null) {
        spdlog::debug("Received component removal for unmapped server entity {} ({})", static_cast<int>(server_entity), type_name<ComponentT>());
        return;
      }

      if (ecs.valid(client_entity)) {
        spdlog::debug("received_component_removal: {} server={} client={}",
                      type_name<ComponentT>(),
                      static_cast<int>(server_entity),
                      static_cast<int>(client_entity));
        ecs.template emplace_or_replace<component_remove_request<ComponentT>>(client_entity, request);
      }
    });
  });
}

// ============================================================================
// Automatic sync setup (client-side observers)
// ============================================================================

template <typename ChannelT, typename... SyncComponentsT>
template <typename ComponentT>
void sync_client_with_channel<ChannelT, SyncComponentsT...>::setup_automatic_sync() {
  using ActualT            = unwrap_hierarchy_t<ComponentT>;
  constexpr bool read_only = is_server_only_v<ComponentT>;

  // Set up for the component itself
  setup_automatic_sync_impl<ActualT, read_only>();

  // Also set up for hierarchy components if wrapped with with_hierarchy<T>
  if constexpr (is_with_hierarchy_v<ComponentT>) {
    setup_automatic_sync_impl<entt_ext::parent<ActualT>, read_only>();
    setup_automatic_sync_impl<entt_ext::children<ActualT>, read_only>();
  }
}

template <typename ChannelT, typename... SyncComponentsT>
template <typename ComponentT, bool ReadOnly>
void sync_client_with_channel<ChannelT, SyncComponentsT...>::setup_automatic_sync_impl() {
  // Set up component observer to track changes
  auto& observer = ecs_.component_observer<ComponentT>();

  if constexpr (!ReadOnly) {
    // When a sync component is added: stamp pending_create<T> first so
    // an offline (or mid-flight failed) creation is recoverable on the
    // next reconcile (phase 4). If we're currently connected and the
    // send succeeds, the marker is cleared immediately so we don't
    // re-upload on reconnect.
    observer.on_construct([this](entt_ext::ecs& ecs, entt_ext::entity e, ComponentT& component) -> asio::awaitable<void> {
      spdlog::debug("Client-side component added: {} {}", type_name<ComponentT>(), static_cast<int>(e));

      if (loading_snapshot_) {
        co_return;
      }
      if (auto request = ecs.template try_get<component_update_request<ComponentT>>(e); request != nullptr) {
        co_return;
      }

      ecs.template emplace_or_replace<pending_create<ComponentT>>(e);

      if (!is_connected()) {
        co_return;
      }

      auto sync_version = std::chrono::steady_clock::now();

      // Spawn the RPC on a separate executor so the channel processor is
      // not blocked for the round-trip. If we co_await here, every
      // observer body for every emplaced sync component pauses the
      // single-threaded process_command_channel until the server replies,
      // and inbound notifications (60Hz steady state) saturate the buffer.
      // The pending_create<T> marker handles retry on RPC failure.
      // mutable: send_component_to_server takes a non-const ComponentT&,
      // so the captured comp must be writable inside the lambda body.
      asio::co_spawn(
          ecs.concurrent_io_context(),
          [this, e, comp = component, sync_version]() mutable -> asio::awaitable<void> {
            try {
              co_await send_component_to_server<ComponentT>(e, comp, sync_version);
              ecs_.defer([e](entt_ext::ecs& ecs_ref) {
                if (ecs_ref.valid(e) && ecs_ref.template all_of<pending_create<ComponentT>>(e)) {
                  ecs_ref.template remove<pending_create<ComponentT>>(e);
                }
              });
            } catch (std::exception const& ex) {
              spdlog::error("Error sending component to server: {} (left as pending_create)", ex.what());
            } catch (...) {
              spdlog::error("Error sending component to server: unknown exception (left as pending_create)");
            }
          },
          asio::detached);

      co_return;
    });

    // When a sync component is updated: stamp pending_update<T>{at_ms},
    // then try to send. Same clear-on-success / leave-on-failure model
    // as on_construct, plus a wall-clock timestamp so the server can
    // resolve last-write-wins conflicts in phase 4.
    observer.on_update([this](entt_ext::ecs& ecs, entt_ext::entity e, ComponentT& component) -> asio::awaitable<void> {
      if (loading_snapshot_) {
        co_return;
      }
      if (auto request = ecs.template try_get<component_update_request<ComponentT>>(e); request != nullptr) {
        co_return;
      }

      auto const now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
      ecs.template emplace_or_replace<pending_update<ComponentT>>(e, pending_update<ComponentT>{now_ms});

      if (!is_connected()) {
        co_return;
      }

      auto sync_version = std::chrono::steady_clock::now();

      // See on_construct above: the RPC is spawned externally so we don't
      // block process_command_channel. pending_update<T> handles retry.
      // mutable: send_component_to_server takes a non-const ComponentT&.
      asio::co_spawn(
          ecs.concurrent_io_context(),
          [this, e, comp = component, sync_version]() mutable -> asio::awaitable<void> {
            try {
              co_await send_component_to_server<ComponentT>(e, comp, sync_version);
              ecs_.defer([e](entt_ext::ecs& ecs_ref) {
                if (ecs_ref.valid(e) && ecs_ref.template all_of<pending_update<ComponentT>>(e)) {
                  ecs_ref.template remove<pending_update<ComponentT>>(e);
                }
              });
            } catch (std::exception const& ex) {
              spdlog::error("Error sending component to server: {} (left as pending_update)", ex.what());
            } catch (...) {
              spdlog::error("Error sending component to server: unknown exception (left as pending_update)");
            }
          },
          asio::detached);

      co_return;
    });

    // When a sync component is removed, notify server immediately
    observer.on_destroy([this](entt_ext::ecs& ecs, entt_ext::entity e, ComponentT& component) -> asio::awaitable<void> {
      if (loading_snapshot_) {
        co_return;
      }
      if (auto request = ecs.template try_get<component_remove_request<ComponentT>>(e); request != nullptr) {
        co_return;
      }

      spdlog::debug("Client-side component destroyed: {} {}", type_name<ComponentT>(), static_cast<int>(e));
      auto sync_version = std::chrono::steady_clock::now();

      // RPC spawned externally — see on_construct/on_update above. No retry
      // marker here because the component has already been destroyed; the
      // server reconciles by version. Errors are logged only.
      asio::co_spawn(
          ecs.concurrent_io_context(),
          [this, e, sync_version]() -> asio::awaitable<void> {
            try {
              co_await notify_component_removal<ComponentT>(e, sync_version);
            } catch (std::exception const& ex) {
              spdlog::error("Error notifying component removal to server: {}", ex.what());
            } catch (...) {
              spdlog::error("Error notifying component removal to server: unknown exception");
            }
          },
          asio::detached);

      co_return;
    });
  } // !ReadOnly

  // Set up observer for component_update_request to apply updates from server
  auto& update_request_observer = ecs_.component_observer<component_update_request<ComponentT>>();

  // Apply an inbound server update. Connected to BOTH on_construct and
  // on_update: the notification handler places the marker with
  // emplace_or_replace, so a second update that arrives before the previous
  // marker is cleared (it is cleared several command cycles later, via the
  // double-defer below) is a *replace* — it fires on_update, not
  // on_construct. Wiring only on_construct silently dropped that second
  // update, leaving the entity stale until the next full snapshot.
  auto apply_component_update =
      [this](entt_ext::ecs& ecs, entt_ext::entity e, component_update_request<ComponentT>& request) -> asio::awaitable<void> {
    // `this` is consumed by `map_entities_async(...)` below, but only on the
    // `if constexpr` branch that runs for hierarchy components or types
    // that opt into entity-id mapping. For ComponentTs where neither
    // branch fires, the capture would otherwise be flagged unused.
    (void)this;
    try {
      // The marker was placed on a valid entity (see notification handler),
      // but this body runs from a co_spawned async observer. Between the
      // marker emplace and here, an entity_destroyed notification or a
      // snapshot reload (reconnect path) can destroy `e`. emplace_or_replace
      // asserts on invalid entities — drop the update silently if it's gone.
      if (!ecs.valid(e)) {
        spdlog::debug("Dropping stale component update for destroyed entity {} ({})", static_cast<int>(e), type_name<ComponentT>());
        co_return;
      }

      // Map entity references from remote to local IDs
      auto component_data = request.component_data;
      if constexpr (is_hierarchy_component<ComponentT>::value ||
                    requires(ComponentT& c, continuous_loader_with_mapping<entt::registry> const& l) { c.map_entities(l); }) {
        co_await map_entities_async(component_data);
        // Re-check after the suspension point — entity may have been
        // destroyed while we were mapping.
        if (!ecs.valid(e)) {
          spdlog::debug("Dropping stale component update for destroyed entity {} ({}) after map_entities",
                        static_cast<int>(e),
                        type_name<ComponentT>());
          co_return;
        }
      }

      // Apply the component update.
      //
      // emplace_or_replace synchronously fires dispatch_on_update for T,
      // which (for non-server_only types) queues T's writeable on_update
      // observer onto command_channel_. That observer body checks for the
      // marker (component_update_request<T>) and bails — so the marker
      // must still be present when the body runs.
      //
      // defer/defer_awaitable preserve call order in command_channel_ (see
      // ecs::defer's contract), so a plain ecs.defer(remove_marker) here
      // lands BEHIND the just-queued observer body. The historical
      // double-defer was a workaround for an earlier broken ordering and
      // is no longer needed — one defer is enough and halves the channel
      // pressure per inbound update.
      ecs.template emplace_or_replace<ComponentT>(e, component_data);

      ecs.defer([e](entt_ext::ecs& ecs_ref) {
        ecs_ref.template remove<component_update_request<ComponentT>>(e);
      });
    } catch (std::exception const& ex) {
      spdlog::error("Error applying component update request: {}", ex.what());
    } catch (...) {
      spdlog::error("Error applying component update request: unknown exception");
    }

    co_return;
  };
  update_request_observer.on_construct(apply_component_update);
  update_request_observer.on_update(apply_component_update);

  // Set up observer for component_remove_request to apply removals from server
  auto& remove_request_observer = ecs_.component_observer<component_remove_request<ComponentT>>();

  // Connected to BOTH on_construct and on_update for the same reason as
  // apply_component_update above — the marker is placed with emplace_or_replace.
  auto apply_component_remove =
      [this](entt_ext::ecs& ecs, entt_ext::entity e, component_remove_request<ComponentT>& request) -> asio::awaitable<void> {
    (void)request;
    try {
      // Same window as apply_component_update: the entity can be destroyed
      // between marker emplace and this body running. remove<T> asserts
      // on invalid entities — drop the request silently if it's gone.
      if (!ecs.valid(e)) {
        spdlog::debug("Dropping stale component remove for destroyed entity {} ({})", static_cast<int>(e), type_name<ComponentT>());
        co_return;
      }
      ecs.template remove<ComponentT>(e);
      co_await ecs.template remove_deferred<component_remove_request<ComponentT>>(e);
    } catch (std::exception const& ex) {
      spdlog::error("Error applying component remove request: {}", ex.what());
    } catch (...) {
      spdlog::error("Error applying component remove request: unknown exception");
    }

    co_return;
  };
  remove_request_observer.on_construct(apply_component_remove);
  remove_request_observer.on_update(apply_component_remove);
}

// ============================================================================
// Server entity / component send paths
// ============================================================================

template <typename ChannelT, typename... SyncComponentsT>
asio::awaitable<entity> sync_client_with_channel<ChannelT, SyncComponentsT...>::request_server_entity(entity client_entity) {
  if (!rpc_client_.is_connected() || session_id_.empty()) {
    co_return entt_ext::null;
  }

  // Single-flight: coalesce concurrent requests for the SAME client entity so
  // exactly one entity_create RPC is issued. Several synced components emplaced
  // on one fresh entity each reach here before any reply lands; without this
  // each would mint a distinct server entity and the entity's components would
  // scatter across them.
  //
  // Leader-election runs on entity_create_strand_: hopping onto it serializes
  // the inflight-map check/insert without a lock, and the RPC co_await below
  // releases the strand so followers register while the leader waits. The
  // in-flight entry is a steady_timer parked at time_point::max(); the leader
  // cancel()s it to wake every follower, which then read the mapping the leader
  // recorded. Only the inflight map + timer need the strand — and because every
  // timer op (the followers' async_wait, the leader's cancel) runs on it, the
  // plain timer is safe without any extra synchronization; the continuous_loader
  // has its own internal mutex.
  co_await asio::dispatch(entity_create_strand_, asio::use_awaitable);

  // --- on entity_create_strand_: synchronous critical section, no co_await ---
  // Another caller may already have established the mapping.
  if (auto existing = continuous_loader_.to_remote(client_entity); existing != entt_ext::null) {
    co_return existing;
  }

  std::shared_ptr<entity_create_event> event;
  bool                                 is_leader = false;
  if (auto it = entity_create_inflight_.find(client_entity); it != entity_create_inflight_.end()) {
    event = it->second; // follower: a leader is already creating this entity
  } else {
    event     = std::make_shared<entity_create_event>(entity_create_strand_, entity_create_event::time_point::max());
    entity_create_inflight_.emplace(client_entity, event);
    is_leader = true;
  }

  if (!is_leader) {
    // Wait for the leader to finish (it cancel()s the timer), then read the
    // mapping it recorded. The async_wait is initiated here on the strand, so
    // it is ordered against the leader's cancel() — no missed wakeup.
    boost::system::error_code ec;
    co_await event->async_wait(asio::redirect_error(asio::use_awaitable, ec));
    co_return continuous_loader_.to_remote(client_entity);
  }

  // Leader: issue the single entity_create RPC. The co_await releases the strand.
  entity server_entity = entt_ext::null;
  try {
    entity_create_request request{.session_id = session_id_, .client_entity = client_entity};

    auto response = co_await rpc_client_.template invoke<entity_create_response>("entity_create", std::move(request));

    if (response.success) {
      server_entity = response.server_entity;
      continuous_loader_.insert_mapping(server_entity, client_entity);
      spdlog::debug("Requested server entity {} for client entity {}", static_cast<int>(server_entity), static_cast<int>(client_entity));
    } else {
      spdlog::error("Entity creation failed: {}", response.error_message);
    }
  } catch (std::exception const& ex) {
    spdlog::error("Exception requesting server entity: {}", ex.what());
  }

  // Re-enter the strand to drop the in-flight entry, then wake every follower
  // by cancelling the timer. Followers read to_remote() — the mapping just
  // recorded, or null on failure, in which case they fail their send and keep
  // their pending_create<T> marker for retry, exactly as a direct failure would.
  co_await asio::dispatch(entity_create_strand_, asio::use_awaitable);
  entity_create_inflight_.erase(client_entity);
  event->cancel();

  co_return server_entity;
}

template <typename ChannelT, typename... SyncComponentsT>
template <typename ComponentT>
asio::awaitable<void>
sync_client_with_channel<ChannelT, SyncComponentsT...>::send_component_to_server(entity e, ComponentT& component, version_type sync_version) {
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

  // Create a copy of the component for mapping
  ComponentT component_to_send = component;

  if constexpr (is_hierarchy_component<ComponentT>::value ||
                requires(ComponentT& c, continuous_loader_with_mapping<entt::registry> const& l) { c.map_entities_to_remote(l); }) {
    co_await map_component_entities_to_remote_async(component_to_send);
  }

  std::string endpoint_name = "component_updated_" + std::string(type_name<ComponentT>());

  // target_entity is now the server entity
  component_update_request<ComponentT> request{.session_id     = session_id_,
                                               .sync_version   = sync_version,
                                               .target_entity  = server_entity,
                                               .component_data = component_to_send};

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

template <typename ChannelT, typename... SyncComponentsT>
template <typename ComponentT>
asio::awaitable<void> sync_client_with_channel<ChannelT, SyncComponentsT...>::notify_component_removal(entity e, version_type sync_version) {
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
    spdlog::error("Component removal sync failed: {}", response.error_message);
    throw std::runtime_error("Component removal sync failed: " + response.error_message);
  }
  co_return;
}

template <typename ChannelT, typename... SyncComponentsT>
asio::awaitable<void> sync_client_with_channel<ChannelT, SyncComponentsT...>::notify_entity_destruction_to_server(entity       e,
                                                                                                                  version_type sync_version) {

  if (loading_snapshot_) {
    spdlog::debug("Skipping entity destruction notification for client entity {} during snapshot load", static_cast<int>(e));
    co_return;
  }
  // Get server entity for this client entity
  auto server_entity = continuous_loader_.to_remote(e);

  if (server_entity == entt_ext::null) {
    // No server entity mapping exists, nothing to destroy on server
    spdlog::debug("No server entity mapping for client entity {} during entity destruction", static_cast<int>(e));
    co_return;
  }

  if (!rpc_client_.is_connected() || session_id_.empty()) {
    spdlog::debug("Not connected to server, skipping entity destruction notification for client entity {}", static_cast<int>(e));
    co_return;
  }

  spdlog::debug("Notifying server about entity destruction: client={} server={}", static_cast<int>(e), static_cast<int>(server_entity));

  entity_destroy_request request{.session_id = session_id_, .server_entity = server_entity, .sync_version = sync_version};

  auto response = co_await rpc_client_.template invoke<entity_destroy_response>("entity_destroy", std::move(request));

  if (!response.success) {
    spdlog::error("Entity destruction sync failed: {}", response.error_message);
    throw std::runtime_error("Entity destruction sync failed: " + response.error_message);
  }

  spdlog::debug("Entity destruction notification sent successfully: client={} server={}", static_cast<int>(e), static_cast<int>(server_entity));
  co_return;
}

// ============================================================================
// Generic map_entities_async (concept-constrained)
// ============================================================================

template <typename ChannelT, typename... SyncComponentsT>
template <typename ComponentT>
  requires requires(ComponentT& c, continuous_loader_with_mapping<entt::registry> const& l) { c.map_entities(l); } &&
           (!sync::is_hierarchy_component_v<ComponentT>)
asio::awaitable<void> sync_client_with_channel<ChannelT, SyncComponentsT...>::map_entities_async(ComponentT& component) {
  // Create placeholder entities for unmapped server entities so map_entities succeeds
  if constexpr (requires(ComponentT const& c) { c.entity_refs(); }) {
    for (auto remote_entity : component.entity_refs()) {
      if (remote_entity != entt_ext::null && continuous_loader_.map(remote_entity) == entt_ext::null) {
        auto local = ecs_.create();
        continuous_loader_.insert_mapping(remote_entity, local);
        spdlog::warn("Created placeholder entity {} for unmapped server entity {}", static_cast<int>(local), static_cast<int>(remote_entity));
      }
    }
  }
  component.map_entities(continuous_loader_);
  co_return;
}

// ============================================================================
// Handshake / entity-sync setup
// ============================================================================

template <typename ChannelT, typename... SyncComponentsT>
asio::awaitable<bool> sync_client_with_channel<ChannelT, SyncComponentsT...>::perform_handshake(std::string const& client_name,
                                                                                                std::string const& client_version,
                                                                                                std::string const& username,
                                                                                                std::string const& password) {
  try {
    // handshake_error_ doubles as the "server actively rejected us" signal
    // (callers treat non-empty as auth failure, which suppresses auto-retry).
    // Clear any stale value from a previous attempt before starting.
    handshake_error_.clear();

    handshake_request request{.client_name      = client_name,
                              .client_version   = client_version,
                              .protocol_version = protocol_version_,
                              .username         = username,
                              .password         = password,
                              .device_id        = device_id_};

    // Short timeout: a handshake normally completes in <100 ms. If the
    // request gets lost in transit (observed: fresh session receives server
    // broadcasts but the outbound handshake never reaches the server), the
    // default 30 s call timeout turns into a long "Connecting..." stall.
    // Failing fast hands control to the auto-retry, which reconnects on a
    // fresh session and reliably succeeds.
    auto response = co_await rpc_client_.template invoke_with_timeout<handshake_response>(std::chrono::seconds(30), "handshake", std::move(request));

    if (response.success) {
      // Validate protocol version match
      if (!response.protocol_version.empty() && response.protocol_version != protocol_version_) {
        spdlog::error("Protocol version mismatch! Client: {}, Server: {}", protocol_version_, response.protocol_version);
        session_id_.clear();
        co_return false;
      }

      session_id_ = response.session_id;
      auth_role_  = response.role;
      auth_token_ = response.auth_token;
      spdlog::info("Handshake successful - Session: {}, User: {}, Role: {}", session_id_, username, auth_role_);
      co_return true;
    } else {
      spdlog::error("Handshake failed: {}", response.error_message);
      handshake_error_ = response.error_message;
      session_id_.clear();
      co_return false;
    }

  } catch (std::exception const& ex) {
    // Transport-level failure (timeout, dropped socket) — NOT a server
    // rejection. Leave handshake_error_ empty so callers classify this as
    // transient and the auto-retry keeps reconnecting; setting it here used
    // to flag the attempt as auth_failed and wedge the client on the
    // connection screen until a manual retry.
    spdlog::error("Handshake exception: {}", ex.what());
    session_id_.clear();
    co_return false;
  } catch (...) {
    spdlog::error("Handshake unknown exception");
    session_id_.clear();
    co_return false;
  }
}

template <typename ChannelT, typename... SyncComponentsT>
void sync_client_with_channel<ChannelT, SyncComponentsT...>::setup_entity_sync() {
  auto& observer = ecs_.component_observer<entt_ext::entity>();
  observer.on_destroy([this](entt_ext::ecs& ecs, entt_ext::entity e) -> asio::awaitable<void> {
    if (continuous_loader_.contains_local(e)) {
      spdlog::info("sync_client: local entity {} destroyed (mapped→server {}); will notify server + drop mapping",
                   static_cast<int>(e), static_cast<int>(continuous_loader_.to_remote(e)));

      auto sync_version = std::chrono::steady_clock::now();

      // Resolve server_entity NOW (before we remove the mapping) and skip
      // the RPC entirely if we're disconnected or there's no mapping —
      // this matches what notify_entity_destruction_to_server would do
      // internally but avoids putting it inside the spawned coroutine,
      // where it would race with the synchronous mapping removal below.
      auto       server_entity = continuous_loader_.to_remote(e);
      bool const was_synced    = !loading_snapshot_ && server_entity != entt_ext::null;
      bool const should_notify = was_synced && rpc_client_.is_connected() && !session_id_.empty();

      // Remove the mapping synchronously now. The spawned RPC has the
      // server_entity captured by value, so it doesn't depend on the
      // map any more.
      continuous_loader_.remove_mapping_by_local(e);

      if (was_synced && !should_notify) {
        // Offline (or no session): the delete can't reach the server
        // right now. Stamp a tombstone so it survives a process restart
        // and is drained by reconcile_pending_deletes on the next
        // connect (see entt_ext/sync/pending_changes.hpp).
        stamp_pending_delete(server_entity);
      }

      if (should_notify) {
        // Spawn the RPC externally so the channel processor isn't blocked
        // for the round-trip. See the on_construct/on_update comments in
        // setup_automatic_sync_impl for the channel-saturation rationale.
        auto sid = session_id_;
        asio::co_spawn(
            ecs.concurrent_io_context(),
            [this, e, server_entity, sync_version, sid = std::move(sid)]() -> asio::awaitable<void> {
              try {
                entity_destroy_request request{.session_id = sid, .server_entity = server_entity, .sync_version = sync_version};
                auto                   response = co_await rpc_client_.template invoke<entity_destroy_response>("entity_destroy", std::move(request));
                if (!response.success) {
                  spdlog::error("Entity destruction sync failed: {}", response.error_message);
                  stamp_pending_delete(server_entity);
                } else {
                  spdlog::debug("Entity destruction notification sent successfully: client={} server={}",
                                static_cast<int>(e),
                                static_cast<int>(server_entity));
                }
              } catch (std::exception const& ex) {
                spdlog::error("Error notifying entity destruction to server: {} {}", static_cast<int>(e), ex.what());
                stamp_pending_delete(server_entity);
              } catch (...) {
                spdlog::error("Error notifying entity destruction to server: unknown exception");
                stamp_pending_delete(server_entity);
              }
            },
            asio::detached);
      }
    }
    co_return;
  });
}

template <typename ChannelT, typename... SyncComponentsT>
void sync_client_with_channel<ChannelT, SyncComponentsT...>::stamp_pending_delete(entity server_entity) {
  auto const at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  ecs_.defer([server_entity, at_ms](entt_ext::ecs& ecs) {
    ecs.template get_or_emplace<pending_deletes>(ecs.get_global_entity()).entries.push_back({server_entity, at_ms});
  });
}

} // namespace entt_ext::sync
