#pragma once

// Sharding helper for sync_client_with_channel<ChannelT, SyncComponentsT...>.
//
// Problem: the app-side "scaffold" TU (the one that includes
// sync_client_impl.hpp and instantiates the class for its component pack)
// pays the full compile cost of every per-component member function
// template, because the class's non-template orchestrator methods
// (constructor, load_snapshot_from_archive, setup_notification_handlers,
// reconcile_pending_changes) each fold-expand a call across the whole
// pack — e.g. `(setup_automatic_sync<SyncComponentsT>(), ...)`. With
// packs in the 40-50 component range and coroutine/Cereal-heavy bodies,
// that one TU can take on the order of ten minutes, serialized on a
// single core.
//
// Fix: split explicit instantiation of the per-component entry points
// across N "shard" TUs (compiled in parallel by ninja), and tell the
// scaffold TU those instantiations live elsewhere via `extern template`
// so its folds become plain calls instead of re-instantiating everything
// in-place.
//
// Usage (per app):
//   1. In N shard .cpp files, for every component assigned to that shard:
//        #include <entt_ext/sync_client_shard.hpp>
//        ENTT_EXT_SYNC_CLIENT_INSTANTIATE(my_app::sync_client_type, my::ComponentA);
//        ENTT_EXT_SYNC_CLIENT_INSTANTIATE(my_app::sync_client_type, my::ComponentB);
//        ...
//      Any partition of the pack across shards works; order doesn't matter.
//   2. In the ONE scaffold TU (the one with the explicit `template class`
//      instantiation / the one constructing sync_client_type), add
//      ENTT_EXT_SYNC_CLIENT_EXTERN(...) for EVERY component in the pack,
//      BEFORE the class is used/instantiated.
//
// Only the entry points directly reached by a top-level fold expression
// need this treatment — everything each of them calls internally
// (setup_automatic_sync_impl, send_component_to_server,
// notify_component_removal, the notification-handler impls, ...) rides
// along as an implicit instantiation inside whichever TU (shard or
// scaffold) ends up calling the entry point, which is exactly where we
// want the heavy lifting to happen.
#include <entt_ext/sync_client_impl.hpp>

#define ENTT_EXT_SYNC_CLIENT_INSTANTIATE(SyncClientT, ComponentT)                                                        \
  template void SyncClientT::setup_automatic_sync<ComponentT>();                                                         \
  template void SyncClientT::setup_component_notification_handlers<ComponentT>();                                        \
  template void SyncClientT::load_component_and_hierarchy<ComponentT>(cereal::PortableBinaryInputArchive&);              \
  template boost::asio::awaitable<void> SyncClientT::remap_component_and_hierarchy<ComponentT>();                        \
  template boost::asio::awaitable<void> SyncClientT::reconcile_creates_for<ComponentT>(bool&);                           \
  template boost::asio::awaitable<void> SyncClientT::reconcile_updates_for<ComponentT>();                                \
  template void SyncClientT::copy_component_to_server_keyed<ComponentT>(                                                 \
      entt::registry&, std::vector<entt_ext::entity> const&, std::vector<entt_ext::entity> const&)

#define ENTT_EXT_SYNC_CLIENT_EXTERN(SyncClientT, ComponentT)                                                             \
  extern template void SyncClientT::setup_automatic_sync<ComponentT>();                                                  \
  extern template void SyncClientT::setup_component_notification_handlers<ComponentT>();                                 \
  extern template void SyncClientT::load_component_and_hierarchy<ComponentT>(cereal::PortableBinaryInputArchive&);       \
  extern template boost::asio::awaitable<void> SyncClientT::remap_component_and_hierarchy<ComponentT>();                 \
  extern template boost::asio::awaitable<void> SyncClientT::reconcile_creates_for<ComponentT>(bool&);                    \
  extern template boost::asio::awaitable<void> SyncClientT::reconcile_updates_for<ComponentT>();                         \
  extern template void SyncClientT::copy_component_to_server_keyed<ComponentT>(                                         \
      entt::registry&, std::vector<entt_ext::entity> const&, std::vector<entt_ext::entity> const&)
