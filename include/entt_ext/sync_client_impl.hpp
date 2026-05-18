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

#include <stdexcept>

namespace entt_ext::sync {

// ============================================================================
// Constructor
// ============================================================================

template <typename ChannelT, typename... SyncComponentsT>
template <typename... ChannelArgs>
sync_client_with_channel<ChannelT, SyncComponentsT...>::sync_client_with_channel(
    ecs& ecs_instance, ChannelArgs&&... channel_args)
    : ecs_(ecs_instance)
    , protocol_version_(sync_component_list<SyncComponentsT...>::generate_protocol_version())
    //, protocol_version_("sync_v1_")
    , continuous_loader_(ecs_.registry())
    , rpc_client_(std::forward<ChannelArgs>(channel_args)...) {

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
asio::awaitable<bool>
sync_client_with_channel<ChannelT, SyncComponentsT...>::connect(std::string const& host,
                                                                 std::uint16_t      port,
                                                                 std::string const& client_name,
                                                                 std::string const& client_version,
                                                                 std::string const& username,
                                                                 std::string const& password) {
  try {
    // First establish TCP connection
    auto          executor = co_await asio::this_coro::executor;
    tcp::resolver resolver(executor);
    auto          endpoints = co_await resolver.async_resolve(host, std::to_string(port), asio::use_awaitable);
    co_await rpc_client_.connect(*endpoints.begin());

    // Set up notification handlers for real-time sync
    setup_notification_handlers();

    // Perform handshake to get session ID (includes authentication)
    bool handshake_success = co_await perform_handshake(client_name, client_version, username, password);
    if (!handshake_success) {
      // Disconnect on handshake failure
      co_await disconnect();
      co_return false;
    }

    // Phase 4 of the offline-first plan (see docs/offline_first.md):
    // before pulling the server's snapshot, push every pending_create<T>
    // / pending_update<T> stamped on local entities while we were
    // disconnected. This way the server's snapshot — which arrives
    // next — already reflects our offline edits, and the merge in
    // continuous_loader doesn't roll them back. Failures are kept on
    // the entity (the marker stays) so the next reconnect retries.
    co_await reconcile_pending_changes();

    co_await request_snapshot();

    co_return true;
  } catch (...) {
    session_id_.clear();
    throw; // Let the caller see the actual error
  }
}

template <typename ChannelT, typename... SyncComponentsT>
asio::awaitable<bool>
sync_client_with_channel<ChannelT, SyncComponentsT...>::keepalive() {
  if (!rpc_client_.is_connected() || session_id_.empty()) {
    co_return false;
  }
  try {
    sync_keepalive_request request{.session_id = session_id_};
    auto response = co_await rpc_client_.template invoke<sync_keepalive_response>("sync_keepalive", std::move(request));
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
asio::awaitable<bool>
sync_client_with_channel<ChannelT, SyncComponentsT...>::request_snapshot(std::vector<entity> const& entities_of_interest) {
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
asio::awaitable<bool>
sync_client_with_channel<ChannelT, SyncComponentsT...>::connect_and_sync(std::string const&         host,
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
asio::awaitable<bool>
sync_client_with_channel<ChannelT, SyncComponentsT...>::apply_sync_response(sync_response const& response) {
  try {
    if (response.snapshot_data.empty()) {
      co_return false;
    }

    ecs_.defer_async([this, response](entt_ext::ecs& ecs) -> asio::awaitable<void> {
      // Load the snapshot into our ECS
      // Use continuous loader to merge entities without conflicts
      // Note: The snapshot contains server entity IDs that get mapped to client entity IDs
      spdlog::debug("Loading snapshot from server");
      loading_snapshot_ = true;
      grlx::rpc::ibufferstream           istream(&response.snapshot_data[0], response.snapshot_data.size());
      cereal::PortableBinaryInputArchive archive(istream);

      // Single shared snapshot-ingest path (also used by the offline-first
      // restore_cached_snapshot): entities → components → orphans → remap.
      co_await load_snapshot_from_archive(archive);

      ecs_.defer_async([this](entt_ext::ecs&) -> asio::awaitable<void> {
        loading_snapshot_ = false;
        spdlog::debug("Snapshot loaded from server");
        co_return;
      });

      co_return;
    });

    // Update sync state
    auto& state     = ecs_.template get<sync_state>();
    state.last_sync = response.server_timestamp;

    co_return true;

  } catch (...) {
    loading_snapshot_ = false;
    co_return false;
  }
}

// ============================================================================
// Snapshot ingest (shared by live sync and offline-first cache restore)
// ============================================================================

template <typename ChannelT, typename... SyncComponentsT>
asio::awaitable<void>
sync_client_with_channel<ChannelT, SyncComponentsT...>::load_snapshot_from_archive(
    cereal::PortableBinaryInputArchive& archive) {
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
void sync_client_with_channel<ChannelT, SyncComponentsT...>::copy_component_to_server_keyed(
    entt::registry& tmp, std::vector<entity> const& mapped_local,
    std::vector<entity> const& mapped_server) {
  using ActualT = unwrap_hierarchy_t<ComponentT>;

  for (std::size_t i = 0; i < mapped_local.size(); ++i) {
    auto loc = mapped_local[i];
    auto srv = mapped_server[i];
    if (auto* c = ecs_.template try_get<ActualT>(loc)) {
      ActualT value = *c;
      // Translate any entity references the component carries from local
      // to server IDs — same hook the live send path uses.
      if constexpr (requires(ActualT& x, continuous_loader_with_mapping<entt::registry> const& l) {
                      x.map_entities_to_remote(l);
                    }) {
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
void sync_client_with_channel<ChannelT, SyncComponentsT...>::save_cached_snapshot(
    cereal::PortableBinaryOutputArchive& archive) {
  // Gather every synced local entity that already has a server mapping,
  // paired with its server ID. Offline-only entities (no server ID yet)
  // are intentionally not cached — that is the documented phase-2
  // limitation; they will be handled by phase-3 pending-change tracking.
  std::vector<entity> mapped_local;
  std::vector<entity> mapped_server;
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
}

template <typename ChannelT, typename... SyncComponentsT>
asio::awaitable<void>
sync_client_with_channel<ChannelT, SyncComponentsT...>::restore_cached_snapshot(
    cereal::PortableBinaryInputArchive& archive) {
  // Identical ingest path to a live server snapshot — the cache file is
  // shaped exactly like sync_response.snapshot_data.
  co_await load_snapshot_from_archive(archive);
  co_return;
}

// ============================================================================
// Reconcile pending changes
// ============================================================================

template <typename ChannelT, typename... SyncComponentsT>
asio::awaitable<void>
sync_client_with_channel<ChannelT, SyncComponentsT...>::reconcile_pending_changes() {
  // Creates: iterate until no entity changed marker state.
  for (int pass = 0; pass < 16; ++pass) {
    bool progress = false;
    co_await reconcile_creates_helper<SyncComponentsT...>(progress);
    if (!progress) break;
  }

  // Updates: order independent — every entity here is already mapped
  // (it was emplaced from a server snapshot at some prior point), so
  // a single pass is enough.
  co_await reconcile_updates_helper<SyncComponentsT...>();

  co_return;
}

template <typename ChannelT, typename... SyncComponentsT>
template <typename First, typename... Rest>
asio::awaitable<void>
sync_client_with_channel<ChannelT, SyncComponentsT...>::reconcile_creates_helper(bool& progress) {
  co_await reconcile_creates_for<First>(progress);
  if constexpr (sizeof...(Rest) > 0) {
    co_await reconcile_creates_helper<Rest...>(progress);
  }
  co_return;
}

template <typename ChannelT, typename... SyncComponentsT>
template <typename First, typename... Rest>
asio::awaitable<void>
sync_client_with_channel<ChannelT, SyncComponentsT...>::reconcile_updates_helper() {
  co_await reconcile_updates_for<First>();
  if constexpr (sizeof...(Rest) > 0) {
    co_await reconcile_updates_helper<Rest...>();
  }
  co_return;
}

template <typename ChannelT, typename... SyncComponentsT>
template <typename ComponentT>
asio::awaitable<void>
sync_client_with_channel<ChannelT, SyncComponentsT...>::reconcile_creates_for(bool& progress) {
  using ActualT            = unwrap_hierarchy_t<ComponentT>;
  constexpr bool read_only = is_server_only_v<ComponentT>;

  if constexpr (read_only) {
    // server_only<T> components are never written by the client.
    co_return;
  } else {
    // Snapshot the entity list — the inner send_component_to_server
    // mutates the registry (mapping update + marker removal) and we
    // can't iterate a view through that.
    std::vector<entity> targets;
    for (auto e : ecs_.template view<pending_create<ActualT>>()) {
      targets.push_back(e);
    }

    for (auto e : targets) {
      if (!ecs_.valid(e)) {
        continue;
      }
      auto* component = ecs_.template try_get<ActualT>(e);
      if (component == nullptr) {
        // Component was removed locally between observer and reconcile;
        // drop the now-stale marker and move on.
        ecs_.template remove<pending_create<ActualT>>(e);
        progress = true;
        continue;
      }

      try {
        co_await send_component_to_server<ActualT>(e, *component, std::chrono::steady_clock::now());
        ecs_.template remove<pending_create<ActualT>>(e);
        progress = true;
      } catch (std::exception const& ex) {
        spdlog::warn("[reconcile] create failed for {} entity {}: {}",
                     type_name<ActualT>(), static_cast<int>(e), ex.what());
      } catch (...) {
        spdlog::warn("[reconcile] create failed for {} entity {}: unknown",
                     type_name<ActualT>(), static_cast<int>(e));
      }
    }
  }
  co_return;
}

template <typename ChannelT, typename... SyncComponentsT>
template <typename ComponentT>
asio::awaitable<void>
sync_client_with_channel<ChannelT, SyncComponentsT...>::reconcile_updates_for() {
  using ActualT            = unwrap_hierarchy_t<ComponentT>;
  constexpr bool read_only = is_server_only_v<ComponentT>;

  if constexpr (read_only) {
    co_return;
  } else {
    std::vector<entity> targets;
    for (auto e : ecs_.template view<pending_update<ActualT>>()) {
      targets.push_back(e);
    }

    for (auto e : targets) {
      if (!ecs_.valid(e)) {
        continue;
      }
      auto* component = ecs_.template try_get<ActualT>(e);
      if (component == nullptr) {
        ecs_.template remove<pending_update<ActualT>>(e);
        continue;
      }

      try {
        co_await send_component_to_server<ActualT>(e, *component, std::chrono::steady_clock::now());
        ecs_.template remove<pending_update<ActualT>>(e);
      } catch (std::exception const& ex) {
        spdlog::warn("[reconcile] update failed for {} entity {}: {}",
                     type_name<ActualT>(), static_cast<int>(e), ex.what());
      } catch (...) {
        spdlog::warn("[reconcile] update failed for {} entity {}: unknown",
                     type_name<ActualT>(), static_cast<int>(e));
      }
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

      try {
        co_await send_component_to_server<ComponentT>(e, component, sync_version);
        ecs.template remove<pending_create<ComponentT>>(e);
      } catch (std::exception const& ex) {
        spdlog::error("Error sending component to server: {} (left as pending_create)", ex.what());
      } catch (...) {
        spdlog::error("Error sending component to server: unknown exception (left as pending_create)");
      }

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

      auto const now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
      ecs.template emplace_or_replace<pending_update<ComponentT>>(e, pending_update<ComponentT>{now_ms});

      if (!is_connected()) {
        co_return;
      }

      auto sync_version = std::chrono::steady_clock::now();

      try {
        co_await send_component_to_server<ComponentT>(e, component, sync_version);
        ecs.template remove<pending_update<ComponentT>>(e);
      } catch (std::exception const& ex) {
        spdlog::error("Error sending component to server: {} (left as pending_update)", ex.what());
      } catch (...) {
        spdlog::error("Error sending component to server: unknown exception (left as pending_update)");
      }

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

      try {
        co_await notify_component_removal<ComponentT>(e, sync_version);
      } catch (std::exception const& ex) {
        spdlog::error("Error notifying component removal to server: {}", ex.what());
      } catch (...) {
        spdlog::error("Error notifying component removal to server: unknown exception");
      }

      co_return;
    });
  } // !ReadOnly

  // Set up observer for component_update_request to apply updates from server
  auto& update_request_observer = ecs_.component_observer<component_update_request<ComponentT>>();

  update_request_observer.on_construct(
      [this](entt_ext::ecs& ecs, entt_ext::entity e, component_update_request<ComponentT>& request) -> asio::awaitable<void> {
        // `this` is consumed by `map_entities_async(...)` below, but only on the
        // `if constexpr` branch that runs for hierarchy components or types
        // that opt into entity-id mapping. For ComponentTs where neither
        // branch fires, the capture would otherwise be flagged unused.
        (void)this;
        try {
          // Map entity references from remote to local IDs
          auto component_data = request.component_data;
          if constexpr (is_hierarchy_component<ComponentT>::value ||
                        requires(ComponentT& c, continuous_loader_with_mapping<entt::registry> const& l) { c.map_entities(l); }) {
            co_await map_entities_async(component_data);
          }

          // Apply the component update
          // Note: emplace_or_replace triggers on_update which queues an async handler.
          // We must NOT remove the request marker until after that async handler has
          // had a chance to check it. Since both handlers go through the same command
          // channel, we use defer_async with a double-defer pattern to ensure the
          // removal happens after the on_update handler completes.
          ecs.template emplace_or_replace<ComponentT>(e, component_data);

          // First defer: This gets queued after the on_update async handler
          // Second defer inside: This ensures removal happens after on_update executes
          ecs.defer_async([e](entt_ext::ecs& ecs_ref) -> asio::awaitable<void> {
            ecs_ref.defer([e](entt_ext::ecs& ecs_inner) {
              ecs_inner.template remove<component_update_request<ComponentT>>(e);
            });
            co_return;
          });
        } catch (std::exception const& ex) {
          spdlog::error("Error applying component update request: {}", ex.what());
        } catch (...) {
          spdlog::error("Error applying component update request: unknown exception");
        }

        co_return;
      });

  // Set up observer for component_remove_request to apply removals from server
  auto& remove_request_observer = ecs_.component_observer<component_remove_request<ComponentT>>();

  remove_request_observer.on_construct(
      [this](entt_ext::ecs& ecs, entt_ext::entity e, component_remove_request<ComponentT>& request) -> asio::awaitable<void> {
        try {
          ecs.template remove<ComponentT>(e);
          co_await ecs.template remove_deferred<component_remove_request<ComponentT>>(e);
        } catch (std::exception const& ex) {
          spdlog::error("Error applying component remove request: {}", ex.what());
        } catch (...) {
          spdlog::error("Error applying component remove request: unknown exception");
        }

        co_return;
      });
}

// ============================================================================
// Server entity / component send paths
// ============================================================================

template <typename ChannelT, typename... SyncComponentsT>
asio::awaitable<entity>
sync_client_with_channel<ChannelT, SyncComponentsT...>::request_server_entity(entity client_entity) {
  if (!rpc_client_.is_connected() || session_id_.empty()) {
    co_return entt_ext::null;
  }

  try {
    entity_create_request request{.session_id = session_id_, .client_entity = client_entity};

    auto response = co_await rpc_client_.template invoke<entity_create_response>("entity_create", std::move(request));

    if (!response.success) {
      spdlog::error("Entity creation failed: {}", response.error_message);
      co_return entt_ext::null;
    }

    continuous_loader_.insert_mapping(response.server_entity, client_entity);

    spdlog::debug("Requested server entity {} for client entity {}", static_cast<int>(response.server_entity), static_cast<int>(client_entity));

    co_return response.server_entity;

  } catch (std::exception const& ex) {
    spdlog::error("Exception requesting server entity: {}", ex.what());
    co_return entt_ext::null;
  }
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
asio::awaitable<void>
sync_client_with_channel<ChannelT, SyncComponentsT...>::notify_component_removal(entity e, version_type sync_version) {
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
asio::awaitable<void>
sync_client_with_channel<ChannelT, SyncComponentsT...>::notify_entity_destruction_to_server(entity e, version_type sync_version) {

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
asio::awaitable<void>
sync_client_with_channel<ChannelT, SyncComponentsT...>::map_entities_async(ComponentT& component) {
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
asio::awaitable<bool>
sync_client_with_channel<ChannelT, SyncComponentsT...>::perform_handshake(std::string const& client_name,
                                                                           std::string const& client_version,
                                                                           std::string const& username,
                                                                           std::string const& password) {
  try {
    handshake_request request{.client_name      = client_name,
                              .client_version   = client_version,
                              .protocol_version = protocol_version_,
                              .username         = username,
                              .password         = password};

    auto response = co_await rpc_client_.template invoke<handshake_response>("handshake", std::move(request));

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
    spdlog::error("Handshake exception: {}", ex.what());
    handshake_error_ = ex.what();
    session_id_.clear();
    co_return false;
  } catch (...) {
    spdlog::error("Handshake unknown exception");
    handshake_error_ = "Unknown handshake error";
    session_id_.clear();
    co_return false;
  }
}

template <typename ChannelT, typename... SyncComponentsT>
void sync_client_with_channel<ChannelT, SyncComponentsT...>::setup_entity_sync() {
  auto& observer = ecs_.component_observer<entt_ext::entity>();
  observer.on_destroy([this](entt_ext::ecs& ecs, entt_ext::entity e) -> asio::awaitable<void> {
    if (continuous_loader_.contains_local(e)) {
      spdlog::debug("Entity destroyed, notifying server and removing mapping: {}", static_cast<int>(e));

      auto sync_version = std::chrono::steady_clock::now();

      try {
        co_await notify_entity_destruction_to_server(e, sync_version);
      } catch (std::exception const& ex) {
        spdlog::error("Error notifying entity destruction to server: {} {}", static_cast<int>(e), ex.what());
      } catch (...) {
        spdlog::error("Error notifying entity destruction to server: unknown exception");
      }

      // Remove entity mapping after notifying server
      continuous_loader_.remove_mapping_by_local(e);
    }
    co_return;
  });
}

} // namespace entt_ext::sync
