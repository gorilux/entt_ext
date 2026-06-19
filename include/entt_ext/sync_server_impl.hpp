#pragma once

// Out-of-line implementations of sync_server_with_channel<...> member
// functions. Auto-included from sync_server.hpp unless the build system
// defines ENTT_EXT_SYNC_SERVER_NO_IMPL — in which case this header should
// be included only from a single per-app instantiation TU that performs
// `template class sync_server_with_channel<...>` for the app's pack.

#include <entt_ext/sync_server.hpp>

#include <cereal/types/string.hpp>
#include <cereal/types/unordered_map.hpp>
#include <cereal/types/unordered_set.hpp>
#include <cereal/types/vector.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <random>
#include <sstream>

namespace entt_ext::sync {

// ============================================================================
// Constructor
// ============================================================================

template <typename ChannelT, typename... SyncComponentsT>
template <typename... ChannelArgs>
sync_server_with_channel<ChannelT, SyncComponentsT...>::sync_server_with_channel(
    entt_ext::ecs& ecs_instance, ChannelArgs&&... channel_args)
    : ecs_(ecs_instance)
    , protocol_version_(sync_component_list<SyncComponentsT...>::generate_protocol_version())
    , rpc_server_(std::forward<ChannelArgs>(channel_args)...) {
  // Set up RPC endpoints
  setup_rpc_endpoints(ecs_instance);

  // Set up automatic synchronization for each component type
  (setup_automatic_sync<SyncComponentsT>(ecs_instance), ...);

  spdlog::info("Sync server initialized with protocol version: {}", protocol_version_);
}

// ============================================================================
// Public RPC handlers
// ============================================================================

template <typename ChannelT, typename... SyncComponentsT>
asio::awaitable<entity_create_response>
sync_server_with_channel<ChannelT, SyncComponentsT...>::handle_entity_create(entity_create_request const& request) {
  try {
    touch_session(request.session_id);

    // Create new server entity
    entity server_entity = ecs_.create();

    spdlog::info("Created server entity {} for client entity {} (session {})",
                 static_cast<int>(server_entity),
                 static_cast<int>(request.client_entity),
                 request.session_id);

    co_return entity_create_response{.success = true, .server_entity = server_entity, .error_message = ""};

  } catch (std::exception const& ex) {
    spdlog::error("Error creating entity: {}", ex.what());
    co_return entity_create_response{.success = false, .server_entity = entt_ext::null, .error_message = std::string("Exception: ") + ex.what()};
  }
}

template <typename ChannelT, typename... SyncComponentsT>
asio::awaitable<entity_destroy_response>
sync_server_with_channel<ChannelT, SyncComponentsT...>::handle_entity_destroy(entity_destroy_request const& request) {
  try {
    touch_session(request.session_id);

    entity server_entity = request.server_entity;

    spdlog::info("Destroying server entity {} (session {}, version {})",
                 static_cast<int>(server_entity),
                 request.session_id,
                 std::chrono::duration_cast<std::chrono::milliseconds>(request.sync_version.time_since_epoch()).count());

    // Validate server entity exists
    if (!ecs_.valid(server_entity)) {
      std::string error_msg = "Server entity does not exist";
      spdlog::warn("{}: {}", error_msg, static_cast<int>(server_entity));
      co_return entity_destroy_response{.success = false, .error_message = error_msg};
    }

    // Notify other clients about the entity destruction before destroying it
    try {
      co_await notify_entity_destruction_to_other_clients(server_entity, request.sync_version, request.session_id);
    } catch (std::exception const& ex) {
      spdlog::error("Error notifying entity destruction to other clients: {}", ex.what());
    } catch (...) {
      spdlog::error("Error notifying entity destruction to other clients: unknown exception");
    }

    // Destroy the server entity
    co_await ecs_.destroy_deferred(server_entity);

    spdlog::debug("Server entity {} destroyed successfully", static_cast<int>(server_entity));

    co_return entity_destroy_response{.success = true, .error_message = ""};

  } catch (std::exception const& ex) {
    spdlog::error("Error destroying entity: {}", ex.what());
    co_return entity_destroy_response{.success = false, .error_message = std::string("Exception: ") + ex.what()};
  }
}

template <typename ChannelT, typename... SyncComponentsT>
asio::awaitable<handshake_response>
sync_server_with_channel<ChannelT, SyncComponentsT...>::handle_handshake_request(handshake_request const& request) {
  try {
    spdlog::info("sync_server::handshake: request from user='{}' client='{}' v{} protocol='{}'",
                 request.username.empty() ? "anonymous" : request.username,
                 request.client_name.empty() ? "unknown" : request.client_name,
                 request.client_version.empty() ? "unknown" : request.client_version,
                 request.protocol_version.empty() ? "(none)" : request.protocol_version);

    // Validate protocol version
    if (!request.protocol_version.empty() && request.protocol_version != protocol_version_) {
      std::string error_msg = "Protocol version mismatch! Server: " + protocol_version_ + ", Client: " + request.protocol_version;
      spdlog::error("sync_server::handshake: {}", error_msg);
      co_return handshake_response{.success          = false,
                                   .session_id       = "",
                                   .error_message    = error_msg,
                                   .protocol_version = protocol_version_,
                                   .server_timestamp = std::chrono::steady_clock::now()};
    }

    // Run authentication if handler is set
    int         auth_role  = 0;
    std::string auth_token;
    if (auth_handler_) {
      auto auth_result = auth_handler_(request);
      if (!auth_result.success) {
        spdlog::warn("sync_server::handshake: authentication failed for user '{}': {}",
                     request.username, auth_result.error_message);
        auth_result.protocol_version = protocol_version_;
        auth_result.server_timestamp = std::chrono::steady_clock::now();
        co_return auth_result;
      }
      auth_role  = auth_result.role;
      auth_token = auth_result.auth_token;
    }

    // Generate unique session ID
    std::string session_id = generate_session_id();

    // Phase A v2: tag the calling rpc::session with the session id so
    // server::notify_session can later target it for per-tenant
    // notification delivery. current_call_context() returns the
    // active dispatch ctx for the synchronous prefix of this
    // coroutine; we read it before any co_await so the thread-local
    // is still valid (see grlx/rpc/security.hpp).
    if (auto const* ctx = grlx::rpc::current_call_context();
        ctx != nullptr && ctx->set_logical_session_id) {
      ctx->set_logical_session_id(session_id);
    }

    // Bind the client's stable device_id to this session now that auth has
    // succeeded (we only reach this point past the auth gate above). From here
    // on, application handlers read current_call_context()->logical_device_id to
    // authorize per-share access against peer_acl, instead of trusting a
    // per-request device_id field that any authenticated caller could forge.
    if (auto const* ctx = grlx::rpc::current_call_context();
        ctx != nullptr && ctx->set_logical_device_id) {
      ctx->set_logical_device_id(request.device_id);
    }

    // Create client state for this session and stamp the multi-tenant
    // identity from the auth result so the snapshot + notification
    // filters can use it later (see docs/multi_tenant.md).
    auto& client_state    = get_or_create_client_state(session_id);
    client_state.user_id  = request.username;
    client_state.role     = auth_role;

    spdlog::info("sync_server::handshake: client connected - Name: {}, User: {}, Role: {}, Version: {}, Session: {}",
                 request.client_name.empty() ? "unknown" : request.client_name,
                 request.username.empty() ? "anonymous" : request.username,
                 auth_role,
                 request.client_version.empty() ? "unknown" : request.client_version,
                 session_id);

    co_return handshake_response{.success          = true,
                                 .session_id       = session_id,
                                 .error_message    = "",
                                 .protocol_version = protocol_version_,
                                 .server_timestamp = std::chrono::steady_clock::now(),
                                 .role             = auth_role,
                                 .auth_token       = auth_token};

  } catch (std::exception const& ex) {
    spdlog::error("sync_server::handshake: failed for user='{}' — {}",
                  request.username.empty() ? "anonymous" : request.username, ex.what());
    co_return handshake_response{.success          = false,
                                 .session_id       = "",
                                 .error_message    = std::string("Handshake failed: ") + ex.what(),
                                 .protocol_version = protocol_version_,
                                 .server_timestamp = std::chrono::steady_clock::now()};
  }
}

template <typename ChannelT, typename... SyncComponentsT>
asio::awaitable<sync_response>
sync_server_with_channel<ChannelT, SyncComponentsT...>::handle_sync_request(sync_request const& request) {
  // Use session ID from request
  std::string const& client_id = request.session_id;
  if (client_id.empty()) {
    // Return empty response for invalid session
    co_return sync_response{.server_timestamp = std::chrono::steady_clock::now(), .snapshot_data = {}};
  }

  auto& client_state = get_or_create_client_state(client_id);

  // Create snapshot of requested entities with sync-enabled components
  grlx::rpc::buffer_type snapshot_buffer;

  try {
    grlx::rpc::ovectorstream            ostream;
    cereal::PortableBinaryOutputArchive archive(ostream);

    // Multi-tenant filter (see docs/multi_tenant.md): build a
    // temporary registry containing only entities visible to this
    // session, with their actual server IDs preserved via
    // create(hint), and snapshot that. The entity table on the wire
    // is therefore naturally filtered — no count or id-range leak.
    auto visible = collect_visible_entities(client_state.user_id, client_state.role);

    entt::registry tmp_reg;
    build_filtered_registry<SyncComponentsT...>(tmp_reg, visible);

    entt::snapshot{tmp_reg}.get<entity>(archive);
    (save_component_and_hierarchy_from<SyncComponentsT>(tmp_reg, archive), ...);

    ostream.swap_vector(snapshot_buffer);

    // Update client's sync state
    client_state.last_sync = std::chrono::steady_clock::now();
    client_state.dirty_entities.clear();

  } catch (...) {
    // Return empty response on error
    snapshot_buffer.clear();
  }

  co_return sync_response{.server_timestamp = std::chrono::steady_clock::now(), .snapshot_data = std::move(snapshot_buffer)};
}

template <typename ChannelT, typename... SyncComponentsT>
asio::awaitable<sync_keepalive_response>
sync_server_with_channel<ChannelT, SyncComponentsT...>::handle_sync_keepalive(sync_keepalive_request const& request) {
  if (request.session_id.empty()) {
    co_return sync_keepalive_response{.success = false, .error_message = "Empty session_id"};
  }
  // touch_session is a no-op when the session is unknown — surface that to
  // the client as an error so it can re-handshake instead of believing the
  // session is healthy.
  if (lookup_session_identity(request.session_id) == nullptr) {
    co_return sync_keepalive_response{.success = false, .error_message = "Unknown session"};
  }
  touch_session(request.session_id);
  co_return sync_keepalive_response{.success = true, .error_message = ""};
}

template <typename ChannelT, typename... SyncComponentsT>
template <typename ComponentT>
asio::awaitable<component_update_response<ComponentT>>
sync_server_with_channel<ChannelT, SyncComponentsT...>::handle_component_update(component_update_request<ComponentT> const& request) {
  try {
    touch_session(request.session_id);

    spdlog::debug("Handling component {} update: {} {}",
                  type_name<ComponentT>(),
                  static_cast<int>(request.target_entity),
                  std::chrono::duration_cast<std::chrono::milliseconds>(request.sync_version.time_since_epoch()).count());

    // target_entity is always the server entity
    entity server_entity = request.target_entity;

    // Validate server entity exists
    if (!ecs_.valid(server_entity)) {
      co_return component_update_response<ComponentT>{.success = false, .error_message = "Server entity does not exist"};
    }

    // Multi-tenant write authorization (see docs/multi_tenant.md). Done
    // synchronously here — not in the deferred observer — so a rejected
    // write returns success=false on the wire. Otherwise the handler
    // already replied success=true by the time the observer ran, leaving
    // the client convinced its write landed when in fact it was dropped.
    // A nullptr requester (session evicted or never existed) is treated
    // the same as a non-owner non-admin: reject. The client should react
    // by re-handshaking; for now it just sees the failure.
    auto const* requester = lookup_session_identity(request.session_id);
    if (auto* existing = ecs_.template try_get<owner>(server_entity)) {
      bool const is_admin   = requester != nullptr && requester->role == 1;
      bool const is_owner   = requester != nullptr && existing->user_id == requester->user_id;
      bool const is_unowned = existing->user_id.empty();
      if (!is_admin && !is_owner && !is_unowned) {
        spdlog::warn("Multi-tenant: rejecting write to {} entity {} owned by '{}' from session {} (user '{}')",
                     type_name<ComponentT>(), static_cast<int>(server_entity),
                     existing->user_id, request.session_id,
                     requester != nullptr ? requester->user_id : std::string{"<unknown>"});
        co_return component_update_response<ComponentT>{.success = false, .error_message = "Not authorized"};
      }
    } else if (requester != nullptr && !requester->user_id.empty()) {
      // First write to an unowned entity by an authenticated user —
      // stamp ownership so subsequent writes from other tenants are
      // rejected at the check above.
      ecs_.template emplace<owner>(server_entity, owner{requester->user_id});
    }

    co_await ecs_.template emplace_or_replace_deferred<component_update_request<ComponentT>>(server_entity, request);

    spdlog::debug("Component {} update handled: {} {}",
                  type_name<ComponentT>(),
                  static_cast<int>(server_entity),
                  std::chrono::duration_cast<std::chrono::milliseconds>(request.sync_version.time_since_epoch()).count());

    co_return component_update_response<ComponentT>{.success = true, .error_message = ""};

  } catch (std::exception const& ex) {
    spdlog::error("Error handling component {} update:  {}", type_name<ComponentT>(), ex.what());
    co_return component_update_response<ComponentT>{.success = false, .error_message = std::string("Exception: ") + ex.what()};
  }
}

template <typename ChannelT, typename... SyncComponentsT>
template <typename ComponentT>
asio::awaitable<component_remove_response<ComponentT>>
sync_server_with_channel<ChannelT, SyncComponentsT...>::handle_component_remove(component_remove_request<ComponentT> const& request) {
  try {
    touch_session(request.session_id);

    // target_entity is always the server entity
    entity server_entity = request.target_entity;

    spdlog::debug("Handling component removal: {} {} {}", type_name<ComponentT>(), request.session_id, static_cast<int>(server_entity));

    // Validate server entity exists
    if (!ecs_.valid(server_entity)) {
      co_return component_remove_response<ComponentT>{.success = false, .error_message = "Server entity does not exist"};
    }

    spdlog::debug("Removing component {} from server entity: {}", type_name<ComponentT>(), static_cast<int>(server_entity));
    co_await ecs_.template remove_deferred<ComponentT>(server_entity);

    co_return component_remove_response<ComponentT>{.success = true, .error_message = ""};

  } catch (std::exception const& ex) {
    co_return component_remove_response<ComponentT>{.success = false, .error_message = std::string("Exception: ") + ex.what()};
  }
}

template <typename ChannelT, typename... SyncComponentsT>
std::size_t sync_server_with_channel<ChannelT, SyncComponentsT...>::cleanup_disconnected_clients() {
  auto const  now     = std::chrono::steady_clock::now();
  std::size_t evicted = 0;
  for (auto it = client_states_.begin(); it != client_states_.end();) {
    if (now - it->second.last_sync > client_idle_timeout_) {
      spdlog::info("ECS-sync: evicting stale client {} (idle for {}s, dirty_entities={})",
                   it->first,
                   std::chrono::duration_cast<std::chrono::seconds>(now - it->second.last_sync).count(),
                   it->second.dirty_entities.size());
      it = client_states_.erase(it);
      ++evicted;
    } else {
      ++it;
    }
  }
  return evicted;
}

template <typename ChannelT, typename... SyncComponentsT>
void sync_server_with_channel<ChannelT, SyncComponentsT...>::remove_client(std::string const& client_id) {
  auto it = client_states_.find(client_id);
  if (it != client_states_.end()) {
    // Remove client state
    client_states_.erase(it);
    spdlog::info("Removed client: {}", client_id);
  }
}

// ============================================================================
// Automatic sync setup (server-side observers)
// ============================================================================

template <typename ChannelT, typename... SyncComponentsT>
template <typename ComponentT>
void sync_server_with_channel<ChannelT, SyncComponentsT...>::setup_automatic_sync(entt_ext::ecs& ecs) {
  using ActualT            = unwrap_hierarchy_t<ComponentT>;
  constexpr bool read_only = is_server_only_v<ComponentT>;

  // Set up for the component itself
  setup_automatic_sync_impl<ActualT, read_only>(ecs);

  // Also set up for hierarchy components if wrapped with with_hierarchy<T>
  if constexpr (is_with_hierarchy_v<ComponentT>) {
    setup_automatic_sync_impl<entt_ext::parent<ActualT>, read_only>(ecs);
    setup_automatic_sync_impl<entt_ext::children<ActualT>, read_only>(ecs);
  }
}

template <typename ChannelT, typename... SyncComponentsT>
template <typename ComponentT, bool ReadOnly>
void sync_server_with_channel<ChannelT, SyncComponentsT...>::setup_automatic_sync_impl(entt_ext::ecs& ecs) {
  // Set up component observer to track server-side changes
  auto& observer = ecs.component_observer<ComponentT>();

  // When a sync component is added by the server, notify all clients immediately
  observer.on_construct([this](entt_ext::ecs& ecs, entt_ext::entity e, ComponentT& component) -> asio::awaitable<void> {
    spdlog::debug("Server-side component added: {} {}", type_name<ComponentT>(), static_cast<int>(e));
    if (auto request = ecs.template try_get<component_update_request<ComponentT>>(e); request != nullptr) {
      co_return;
    }

    auto sync_version = std::chrono::steady_clock::now();

    try {
      co_await notify_component_update_to_all_clients<ComponentT>(e, sync_version, component);
    } catch (std::exception const& ex) {
      spdlog::error("Error notifying component update to all clients: {}", ex.what());
    } catch (...) {
      spdlog::error("Error notifying component update to all clients: unknown exception");
    }

    co_return;
  });

  // When a sync component is updated by the server, notify all clients immediately
  observer.on_update([this](entt_ext::ecs& ecs, entt_ext::entity e, ComponentT& component) -> asio::awaitable<void> {
    // spdlog::debug("Server-side component updated: {} {}", type_name<ComponentT>(), static_cast<int>(e));
    if (auto request = ecs.template try_get<component_update_request<ComponentT>>(e); request != nullptr) {
      co_return;
    }

    auto sync_version = std::chrono::steady_clock::now();

    try {
      co_await notify_component_update_to_all_clients<ComponentT>(e, sync_version, component);
    } catch (std::exception const& ex) {
      spdlog::error("Error notifying component update to all clients: {}", ex.what());
    } catch (...) {
      spdlog::error("Error notifying component update to all clients: unknown exception");
    }

    co_return;
  });

  // When a sync component is removed by the server, notify all clients immediately
  observer.on_destroy([this](entt_ext::ecs& ecs, entt_ext::entity e, ComponentT& component) -> asio::awaitable<void> {
    spdlog::debug("Server-side component removed: {} {}", type_name<ComponentT>(), static_cast<int>(e));
    if (auto request = ecs.template try_get<component_remove_request<ComponentT>>(e); request != nullptr) {
      co_return;
    }

    auto sync_version = std::chrono::steady_clock::now();

    try {
      co_await notify_component_removal_to_all_clients<ComponentT>(e, sync_version);
    } catch (std::exception const& ex) {
      spdlog::error("Error notifying component removal to all clients: {}", ex.what());
    } catch (...) {
      spdlog::error("Error notifying component removal to all clients: unknown exception");
    }

    co_return;
  });

  if constexpr (!ReadOnly) {
    // Set up observer for component_update_request to apply updates from clients
    auto& update_request_observer = ecs.component_observer<component_update_request<ComponentT>>();

    // Apply an inbound client update. Connected to BOTH on_construct and
    // on_update: handle_component_update places the marker with
    // emplace_or_replace_deferred, so a second update that arrives before the
    // previous marker is cleared is a *replace* — it fires on_update, not
    // on_construct. Wiring only on_construct silently dropped that second
    // update on the server.
    auto apply_component_update =
        [this](entt_ext::ecs& ecs, entt_ext::entity e, component_update_request<ComponentT>& request) -> asio::awaitable<void> {
          try {
            // Multi-tenant write authorization is enforced synchronously
            // in handle_component_update (see sync_server_impl.hpp,
            // handle_component_update). By the time this observer fires,
            // the request has already been authorized and `owner` stamped
            // if it was a first-write. The check used to live here, but
            // running it after the synchronous handler had already
            // returned success=true meant rejected writes appeared to
            // succeed on the wire.

            // Apply the component update
            spdlog::debug("Applying component update request: {} server={} client={} version={}",
                          type_name<ComponentT>(),
                          static_cast<int>(e),
                          std::chrono::duration_cast<std::chrono::milliseconds>(request.sync_version.time_since_epoch()).count(),
                          request.session_id);
            ecs.template emplace_or_replace<ComponentT>(e, request.component_data);

            // Phase A v2: hierarchy ownership inheritance. When a child
            // is linked to a parent via parent<T>, copy the parent's
            // owner onto the child if the child has no owner yet or
            // its owner is empty. This makes sub-trees consistent so
            // a snapshot for user A includes every descendant of an
            // A-owned root.
            if constexpr (is_parent_v<ComponentT>) {
              auto parent_e = request.component_data.entity;
              if (ecs.valid(parent_e)) {
                if (auto* parent_owner = ecs.template try_get<owner>(parent_e)) {
                  auto* my_owner = ecs.template try_get<owner>(e);
                  if (my_owner == nullptr || my_owner->user_id.empty()) {
                    ecs.template emplace_or_replace<owner>(e, owner{parent_owner->user_id});
                  }
                }
              }
            }

            // Notify all other clients about the component update
            co_await notify_component_update_to_other_clients<ComponentT>(e, request.sync_version, request.component_data, request.session_id);

            // Remove the request marker
            co_await ecs.template remove_deferred<component_update_request<ComponentT>>(e);
          } catch (std::exception const& ex) {
            spdlog::error("Error applying component update request: {}", ex.what());
          } catch (...) {
            spdlog::error("Error applying component update request: unknown exception");
          }

          co_return;
        };
    update_request_observer.on_construct(apply_component_update);
    update_request_observer.on_update(apply_component_update);

    // Set up observer for component_remove_request to apply removals from clients
    auto& remove_request_observer = ecs.component_observer<component_remove_request<ComponentT>>();

    // Connected to BOTH on_construct and on_update for the same reason as
    // apply_component_update above — the marker is placed with emplace_or_replace.
    auto apply_component_remove =
        [this](entt_ext::ecs& ecs, entt_ext::entity e, component_remove_request<ComponentT>& request) -> asio::awaitable<void> {
          try {
            ecs.template remove<ComponentT>(e);
            co_await notify_component_removal_to_other_clients<ComponentT>(e, request.sync_version, request.session_id);
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
  } // !ReadOnly
}

// ============================================================================
// Component update / removal notification helpers
// ============================================================================

template <typename ChannelT, typename... SyncComponentsT>
template <typename ComponentT>
asio::awaitable<void>
sync_server_with_channel<ChannelT, SyncComponentsT...>::notify_component_update_to_all_clients(entity server_entity, version_type sync_version, ComponentT const& component_data) {
  if (!notifications_enabled_) {
    co_return; // Skip notifications if disabled
  }

  std::string endpoint_name = "component_updated_" + std::string(type_name<ComponentT>());

  // session_id is meaningful only for "to other clients" routing; for a
  // global broadcast we leave it empty so the receiver doesn't mistake
  // the notification for an echo of its own change.
  component_update_request<ComponentT> request{.session_id     = std::string{},
                                               .sync_version   = sync_version,
                                               .target_entity  = server_entity,
                                               .component_data = component_data};

  try {
    co_await rpc_server_.notify(endpoint_name, std::move(request));
  } catch (std::exception const& ex) {
    spdlog::error("notify_component_update_to_all_clients({}): {}", type_name<ComponentT>(), ex.what());
  } catch (...) {
    spdlog::error("notify_component_update_to_all_clients({}): unknown exception", type_name<ComponentT>());
  }
}

template <typename ChannelT, typename... SyncComponentsT>
template <typename ComponentT>
asio::awaitable<void>
sync_server_with_channel<ChannelT, SyncComponentsT...>::notify_component_update_to_client(entity server_entity, version_type sync_version, ComponentT const& component, std::string const& client_id) {
  // spdlog::debug("Notifying component update to client: {} server_entity={} client={} {}",
  //               type_name<ComponentT>(),
  //               static_cast<int>(server_entity),
  //               client_id,
  //               std::chrono::duration_cast<std::chrono::milliseconds>(sync_version.time_since_epoch()).count());
  std::string endpoint_name = "component_updated_" + std::string(type_name<ComponentT>());

  // Send server entity in target_entity field
  component_update_request<ComponentT> request{.session_id     = client_id,
                                               .sync_version   = sync_version,
                                               .target_entity  = server_entity,
                                               .component_data = component};

  co_await rpc_server_.notify(endpoint_name, std::move(request));

  co_return;
}

template <typename ChannelT, typename... SyncComponentsT>
template <typename ComponentT>
asio::awaitable<void>
sync_server_with_channel<ChannelT, SyncComponentsT...>::notify_component_removal_to_client(entity server_entity, version_type sync_version, std::string const& client_id) {
  spdlog::debug("Notifying component removal to client: {} server_entity={} client={} {}",
                type_name<ComponentT>(),
                static_cast<int>(server_entity),
                client_id,
                std::chrono::duration_cast<std::chrono::milliseconds>(sync_version.time_since_epoch()).count());

  std::string component_name    = std::string(type_name<ComponentT>());
  std::string notification_name = "component_removed_" + component_name;

  // Send server entity in target_entity field
  component_remove_request<ComponentT> request{.session_id = client_id, .sync_version = sync_version, .target_entity = server_entity};

  try {
    co_await rpc_server_.notify(notification_name, std::move(request));
  } catch (...) {
    // Log error or handle notification failure
  }
  co_return;
}

template <typename ChannelT, typename... SyncComponentsT>
template <typename ComponentT>
asio::awaitable<void>
sync_server_with_channel<ChannelT, SyncComponentsT...>::notify_component_removal_to_all_clients(entity server_entity, version_type sync_version) {
  if (!notifications_enabled_) {
    co_return; // Skip notifications if disabled
  }

  std::string notification_name = "component_removed_" + std::string(type_name<ComponentT>());

  component_remove_request<ComponentT> request{.session_id = std::string{}, .sync_version = sync_version, .target_entity = server_entity};
  try {
    co_await rpc_server_.notify(notification_name, std::move(request));
  } catch (std::exception const& ex) {
    spdlog::error("notify_component_removal_to_all_clients({}): {}", type_name<ComponentT>(), ex.what());
  } catch (...) {
    spdlog::error("notify_component_removal_to_all_clients({}): unknown exception", type_name<ComponentT>());
  }
}

template <typename ChannelT, typename... SyncComponentsT>
template <typename ComponentT>
asio::awaitable<void>
sync_server_with_channel<ChannelT, SyncComponentsT...>::notify_component_update_to_other_clients(entity             server_entity,
                                                                                                  version_type       sync_version,
                                                                                                  ComponentT const&  component_data,
                                                                                                  std::string const& except_client_id) {
  if (!notifications_enabled_) {
    co_return; // Skip notifications if disabled
  }

  std::string component_name    = std::string(type_name<ComponentT>());
  std::string notification_name = "component_updated_" + component_name;

  // Phase A v2: per-tenant targeted delivery via notify_session.
  // Filter sessions by entity ownership so the bytes never leave the
  // server for the wrong tenant.
  for (auto& [client_id, client_state] : client_states_) {
    if (client_id == except_client_id)
      continue;

    if (!is_entity_visible_to(server_entity, client_state.user_id, client_state.role)) {
      continue;
    }

    component_update_request<ComponentT> request{.session_id     = client_id,
                                                 .sync_version   = sync_version,
                                                 .target_entity  = server_entity,
                                                 .component_data = component_data};

    try {
      co_await rpc_server_.notify_session(client_id, notification_name, std::move(request));
    } catch (...) {
      // Continue with other clients
    }
  }
}

template <typename ChannelT, typename... SyncComponentsT>
template <typename ComponentT>
asio::awaitable<void>
sync_server_with_channel<ChannelT, SyncComponentsT...>::notify_component_removal_to_other_clients(entity server_entity, version_type sync_version, std::string const& except_client_id) {
  if (!notifications_enabled_) {
    co_return; // Skip notifications if disabled
  }

  std::string component_name    = std::string(type_name<ComponentT>());
  std::string notification_name = "component_removed_" + component_name;

  // Phase A v2: per-tenant targeted delivery (see notify_component_update_to_other_clients).
  for (auto& [client_id, client_state] : client_states_) {
    if (client_id == except_client_id)
      continue;

    if (!is_entity_visible_to(server_entity, client_state.user_id, client_state.role)) {
      continue;
    }

    component_remove_request<ComponentT> request{.session_id = client_id, .sync_version = sync_version, .target_entity = server_entity};
    try {
      co_await rpc_server_.notify_session(client_id, notification_name, std::move(request));
    } catch (...) {
      // Continue with other clients
    }
  }
}

// ============================================================================
// Entity destruction notification helpers
// ============================================================================

template <typename ChannelT, typename... SyncComponentsT>
asio::awaitable<void>
sync_server_with_channel<ChannelT, SyncComponentsT...>::notify_entity_destruction_to_client(entity server_entity, version_type sync_version, std::string const& client_id) {
  spdlog::debug("Notifying entity destruction to client: server_entity={} client={} version={}",
                static_cast<int>(server_entity),
                client_id,
                std::chrono::duration_cast<std::chrono::milliseconds>(sync_version.time_since_epoch()).count());

  std::string notification_name = "entity_destroyed";

  // Send server entity in the request
  entity_destroy_request request{.session_id = client_id, .server_entity = server_entity, .sync_version = sync_version};

  try {
    co_await rpc_server_.notify(notification_name, std::move(request));
  } catch (std::exception const& ex) {
    spdlog::error("Error notifying entity destruction to client {}: {}", client_id, ex.what());
  } catch (...) {
    spdlog::error("Error notifying entity destruction to client {}: unknown exception", client_id);
  }
  co_return;
}

template <typename ChannelT, typename... SyncComponentsT>
asio::awaitable<void>
sync_server_with_channel<ChannelT, SyncComponentsT...>::notify_entity_destruction_to_all_clients(entity server_entity, version_type sync_version) {
  if (!notifications_enabled_) {
    co_return; // Skip notifications if disabled
  }

  spdlog::debug("Notifying entity destruction to all clients: server_entity={} version={}",
                static_cast<int>(server_entity),
                std::chrono::duration_cast<std::chrono::milliseconds>(sync_version.time_since_epoch()).count());

  entity_destroy_request request{.session_id = std::string{}, .server_entity = server_entity, .sync_version = sync_version};
  try {
    co_await rpc_server_.notify("entity_destroyed", std::move(request));
  } catch (std::exception const& ex) {
    spdlog::error("notify_entity_destruction_to_all_clients: {}", ex.what());
  } catch (...) {
    spdlog::error("notify_entity_destruction_to_all_clients: unknown exception");
  }
  co_return;
}

template <typename ChannelT, typename... SyncComponentsT>
asio::awaitable<void>
sync_server_with_channel<ChannelT, SyncComponentsT...>::notify_entity_destruction_to_other_clients(entity server_entity, version_type sync_version, std::string const& except_client_id) {
  if (!notifications_enabled_) {
    co_return; // Skip notifications if disabled
  }

  std::string notification_name = "entity_destroyed";

  spdlog::debug("Notifying entity destruction to other clients: server_entity={} except_client={} version={}",
                static_cast<int>(server_entity),
                except_client_id,
                std::chrono::duration_cast<std::chrono::milliseconds>(sync_version.time_since_epoch()).count());

  // Notify each client (except the one that made the change) with server entity ID
  for (auto& [client_id, client_state] : client_states_) {
    if (client_id == except_client_id)
      continue;

    spdlog::debug("Notifying entity destruction to client: server_entity={} client={} version={}",
                  static_cast<int>(server_entity),
                  client_id,
                  std::chrono::duration_cast<std::chrono::milliseconds>(sync_version.time_since_epoch()).count());

    // Send notification with server entity ID
    entity_destroy_request request{.session_id = client_id, .server_entity = server_entity, .sync_version = sync_version};
    try {
      co_await rpc_server_.notify(notification_name, std::move(request));
    } catch (std::exception const& ex) {
      spdlog::error("Error notifying entity destruction to client {}: {}", client_id, ex.what());
      // Continue with other clients
    } catch (...) {
      spdlog::error("Error notifying entity destruction to client {}: unknown exception", client_id);
      // Continue with other clients
    }
  }
  co_return;
}

// ============================================================================
// Snapshot helpers
// ============================================================================

template <typename ChannelT, typename... SyncComponentsT>
template <typename ComponentT>
void sync_server_with_channel<ChannelT, SyncComponentsT...>::save_component_and_hierarchy(cereal::PortableBinaryOutputArchive& archive) {
  using ActualT = unwrap_hierarchy_t<ComponentT>;

  // Save the component itself
  entt::snapshot{ecs_.registry()}.template get<ActualT>(archive);

  // Also save hierarchy components if wrapped with with_hierarchy<T>
  if constexpr (is_with_hierarchy_v<ComponentT>) {
    entt::snapshot{ecs_.registry()}.template get<entt_ext::parent<ActualT>>(archive);
    entt::snapshot{ecs_.registry()}.template get<entt_ext::children<ActualT>>(archive);
  }
}

template <typename ChannelT, typename... SyncComponentsT>
template <typename ComponentT>
void sync_server_with_channel<ChannelT, SyncComponentsT...>::save_component_and_hierarchy_filtered(cereal::PortableBinaryOutputArchive& archive,
                                                                                                    std::vector<entity> const&           visible) {
  using ActualT = unwrap_hierarchy_t<ComponentT>;

  entt::snapshot{ecs_.registry()}.template get<ActualT>(archive, visible.begin(), visible.end());

  if constexpr (is_with_hierarchy_v<ComponentT>) {
    entt::snapshot{ecs_.registry()}.template get<entt_ext::parent<ActualT>>(archive, visible.begin(), visible.end());
    entt::snapshot{ecs_.registry()}.template get<entt_ext::children<ActualT>>(archive, visible.begin(), visible.end());
  }
}

template <typename ChannelT, typename... SyncComponentsT>
template <typename ComponentT>
void sync_server_with_channel<ChannelT, SyncComponentsT...>::save_component_and_hierarchy_from(entt::registry&                      reg,
                                                                                                cereal::PortableBinaryOutputArchive& archive) {
  using ActualT = unwrap_hierarchy_t<ComponentT>;

  entt::snapshot{reg}.template get<ActualT>(archive);

  if constexpr (is_with_hierarchy_v<ComponentT>) {
    entt::snapshot{reg}.template get<entt_ext::parent<ActualT>>(archive);
    entt::snapshot{reg}.template get<entt_ext::children<ActualT>>(archive);
  }
}

// ============================================================================
// RPC endpoint registration
// ============================================================================

template <typename ChannelT, typename... SyncComponentsT>
void sync_server_with_channel<ChannelT, SyncComponentsT...>::setup_rpc_endpoints(entt_ext::ecs& ecs) {
  // Register handshake endpoint (no session context needed for handshake)
  rpc_server_.attach("handshake", [this](handshake_request const& request) -> asio::awaitable<handshake_response> {
    co_return co_await handle_handshake_request(request);
  });

  // Register general sync endpoint
  rpc_server_.attach("sync", [this](sync_request const& request) -> asio::awaitable<sync_response> {
    // Note: For now, sync requests don't require session context since they work with anonymous clients
    // In a more complete implementation, you'd extract session_id from request or connection context
    co_return co_await handle_sync_request(request);
  });

  // Register sync-level keepalive endpoint
  rpc_server_.attach("sync_keepalive", [this](sync_keepalive_request const& request) -> asio::awaitable<sync_keepalive_response> {
    co_return co_await handle_sync_keepalive(request);
  });

  // Register entity creation endpoint
  rpc_server_.attach("entity_create", [this](entity_create_request const& request) -> asio::awaitable<entity_create_response> {
    co_return co_await handle_entity_create(request);
  });

  // Register entity destruction endpoint
  rpc_server_.attach("entity_destroy", [this](entity_destroy_request const& request) -> asio::awaitable<entity_destroy_response> {
    co_return co_await handle_entity_destroy(request);
  });

  // Register component-specific endpoints using fold expression
  (register_component_endpoints<SyncComponentsT>(ecs), ...);
}

template <typename ChannelT, typename... SyncComponentsT>
template <typename ComponentT>
void sync_server_with_channel<ChannelT, SyncComponentsT...>::register_component_endpoints(entt_ext::ecs& ecs) {
  using ActualT            = unwrap_hierarchy_t<ComponentT>;
  constexpr bool read_only = is_server_only_v<ComponentT>;

  // Register for the component itself
  register_component_endpoints_impl<ActualT, read_only>(ecs);

  // Also register for hierarchy components if wrapped with with_hierarchy<T>
  if constexpr (is_with_hierarchy_v<ComponentT>) {
    register_component_endpoints_impl<entt_ext::parent<ActualT>, read_only>(ecs);
    register_component_endpoints_impl<entt_ext::children<ActualT>, read_only>(ecs);
  }
}

template <typename ChannelT, typename... SyncComponentsT>
template <typename ComponentT, bool ReadOnly>
void sync_server_with_channel<ChannelT, SyncComponentsT...>::register_component_endpoints_impl(entt_ext::ecs& ecs) {
  std::string component_name = std::string(type_name<ComponentT>());

  // Register update endpoint: "update_component_Position", "update_component_Velocity", etc.
  // This handles both insert and update operations
  std::string update_endpoint = "component_updated_" + component_name;

  if constexpr (ReadOnly) {
    // server_only component — reject client updates
    rpc_server_.attach(update_endpoint,
                       [](component_update_request<ComponentT> const& request) -> asio::awaitable<component_update_response<ComponentT>> {
                         spdlog::warn("Rejected client update for server_only component: {}", type_name<ComponentT>());
                         co_return component_update_response<ComponentT>{.success = false, .error_message = "server_only component"};
                       });
  } else {
    rpc_server_.attach(update_endpoint,
                       [this](component_update_request<ComponentT> const& request) -> asio::awaitable<component_update_response<ComponentT>> {
                         co_return co_await handle_component_update<ComponentT>(request);
                       });
  }

  // Register remove endpoint: "remove_component_Position", "remove_component_Velocity", etc.
  std::string remove_endpoint = "component_removed_" + component_name;

  if constexpr (ReadOnly) {
    // server_only component — reject client removals
    rpc_server_.attach(remove_endpoint,
                       [](component_remove_request<ComponentT> const& request) -> asio::awaitable<component_remove_response<ComponentT>> {
                         spdlog::warn("Rejected client removal for server_only component: {}", type_name<ComponentT>());
                         co_return component_remove_response<ComponentT>{.success = false, .error_message = "server_only component"};
                       });
  } else {
    rpc_server_.attach(remove_endpoint,
                       [this](component_remove_request<ComponentT> const& request) -> asio::awaitable<component_remove_response<ComponentT>> {
                         co_return co_await handle_component_remove<ComponentT>(request);
                       });
  }
}

// ============================================================================
// Misc helpers
// ============================================================================

template <typename ChannelT, typename... SyncComponentsT>
void sync_server_with_channel<ChannelT, SyncComponentsT...>::mark_dirty_capped(client_sync_state& state, entity entt) {
  if (state.full_resync_needed) {
    // Already over the cap; further inserts are wasted work.
    return;
  }
  if (state.dirty_entities.size() >= kDirtyEntitiesHardCap) {
    state.dirty_entities.clear();
    state.full_resync_needed = true;
    spdlog::warn("ECS-sync: client {} exceeded dirty_entities cap; forcing full resync", state.client_id);
    return;
  }
  state.dirty_entities.insert(entt);
}

template <typename ChannelT, typename... SyncComponentsT>
std::string sync_server_with_channel<ChannelT, SyncComponentsT...>::generate_session_id() {
  static std::random_device                      rd;
  static std::mt19937                            gen(rd());
  static std::uniform_int_distribution<uint64_t> dis;

  auto timestamp   = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  auto random_part = dis(gen);

  std::stringstream ss;
  ss << "session_" << std::hex << timestamp << "_" << std::hex << random_part;
  return ss.str();
}

} // namespace entt_ext::sync
