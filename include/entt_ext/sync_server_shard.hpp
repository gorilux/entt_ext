#pragma once

// Server-side counterpart to sync_client_shard.hpp — see that header for
// the full rationale. Splits explicit instantiation of
// sync_server_with_channel<ChannelT, SyncComponentsT...>'s per-component
// entry points across N shard TUs, with `extern template` in the
// scaffold TU so its folds (constructor's setup_automatic_sync,
// handle_sync_request's save_component_and_hierarchy_from,
// setup_rpc_endpoints's register_component_endpoints) don't re-instantiate
// everything locally.
#include <entt_ext/sync_server_impl.hpp>

#define ENTT_EXT_SYNC_SERVER_INSTANTIATE(SyncServerT, ComponentT)                                                        \
  template void SyncServerT::setup_automatic_sync<ComponentT>(entt_ext::ecs&);                                           \
  template void SyncServerT::save_component_and_hierarchy_from<ComponentT>(                                              \
      entt::registry&, cereal::PortableBinaryOutputArchive&);                                                            \
  template void SyncServerT::register_component_endpoints<ComponentT>(entt_ext::ecs&)

#define ENTT_EXT_SYNC_SERVER_EXTERN(SyncServerT, ComponentT)                                                              \
  extern template void SyncServerT::setup_automatic_sync<ComponentT>(entt_ext::ecs&);                                    \
  extern template void SyncServerT::save_component_and_hierarchy_from<ComponentT>(                                       \
      entt::registry&, cereal::PortableBinaryOutputArchive&);                                                            \
  extern template void SyncServerT::register_component_endpoints<ComponentT>(entt_ext::ecs&)
