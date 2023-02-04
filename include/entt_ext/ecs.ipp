#pragma once

namespace entt_ext {

///////// SYSTEM IMPLEMENTATION /////////

template <typename FuncT, run_policy Policy, typename... ComponentsT, typename... ExcludeT>
system::system(run_config<FuncT, Policy, entt::get_t<ComponentsT...>, entt::exclude_t<ExcludeT...>>&& cfg, entt_ext::ecs& ecs) {

  // using ViewT = decltype(std::declval<entt_ext::ecs>().template view<ComponentsT...>(entt::exclude_t<ExcludeT...>{}));

  run = [cfg = std::move(cfg), view = ecs.template view<ComponentsT...>(entt::exclude_t<ExcludeT...>{}), this](system& self, entt_ext::ecs& ecs, double dt) mutable -> asio::awaitable<bool> {
    auto executor = co_await asio::this_coro::executor;

    constexpr run_policy spawn_policy = (Policy == run_policy::automatic || Policy == run_policy::once) ? (std::is_same_v<return_type_t<FuncT>, asio::awaitable<void>> ? run_policy::parallel : run_policy::sequential) : Policy;

    if (view.begin() == view.end()) {
      co_return false;
    }

    if constexpr (Policy == run_policy::once) {
      ecs.template emplace<run_once_tag>(cfg.entity);
    }

    if constexpr (spawn_policy == run_policy::parallel && std::is_same_v<return_type_t<FuncT>, asio::awaitable<void>>) {
      co_await run_parallel(cfg.concurrent_io_ctx, ecs, dt, cfg.handler, view);
    } else if constexpr (spawn_policy == run_policy::sequential) {
      if constexpr (std::is_same_v<return_type_t<FuncT>, void>) {
        std::apply(cfg.handler, std::tie(ecs, dt, view));
      } else if constexpr (std::is_same_v<return_type_t<FuncT>, asio::awaitable<void>>) {
        co_await std::apply(cfg.handler, std::tie(ecs, dt, view));
      }
    } else if constexpr (spawn_policy == run_policy::detached) {
      ecs.template emplace<running<system_tag>>(cfg.entity);
      asio::co_spawn(
          cfg.concurrent_io_ctx,
          [entity = cfg.entity, &main_io_ctx = cfg.main_io_ctx, &ecs, dt, handler = cfg.handler, view]() mutable -> asio::awaitable<void> {
            auto executor = co_await asio::this_coro::executor;
            co_await std::apply(handler, std::tie(ecs, dt, view));
            asio::post(main_io_ctx, [&ecs, entity]() {
              ecs.template remove<running<system_tag>>(entity);
            });
            co_return;
          },
          asio::detached);
    } else {
      throw std::runtime_error("Invalid run_policy");
    }
    co_return true;
  };
}

template <typename FuncT, run_policy Policy, typename... ComponentsT, typename... ExcludeT>
system::system(each_config<FuncT, Policy, entt::get_t<ComponentsT...>, entt::exclude_t<ExcludeT...>>&& cfg, entt_ext::ecs& ecs) {
  // using ViewT = decltype(std::declval<entt_ext::ecs>().template view<ComponentsT...>(entt::exclude_t<ExcludeT...>{}));

  run = [cfg = std::move(cfg), view = ecs.template view<ComponentsT...>(entt::exclude_t<running<FuncT>, running<FuncT, run_once_tag>, ExcludeT...>{}), this](system& self, entt_ext::ecs& ecs, double dt) mutable -> asio::awaitable<bool> {
    auto executor = co_await asio::this_coro::executor;

    constexpr run_policy spawn_policy = (Policy == run_policy::automatic || Policy == run_policy::once) ? (std::is_same_v<return_type_t<FuncT>, asio::awaitable<void>> ? run_policy::parallel : run_policy::sequential) : Policy;

    if (view.begin() == view.end()) {
      co_return false;
    }

    if constexpr (spawn_policy == run_policy::parallel && std::is_same_v<return_type_t<FuncT>, asio::awaitable<void>>) {
      co_await each_parallel(cfg.concurrent_io_ctx, ecs, dt, cfg.handler, view, run_policy_t<Policy>{});
    } else if constexpr (spawn_policy == run_policy::sequential) {
      for (auto entry : view.each()) {
        if constexpr (std::is_same_v<return_type_t<FuncT>, void>) {
          std::apply(cfg.handler, std::tuple_cat(std::tie(ecs, dt), entry));
        } else if constexpr (std::is_same_v<return_type_t<FuncT>, asio::awaitable<void>>) {
          co_await std::apply(cfg.handler, std::tuple_cat(std::tie(ecs, dt), entry));
        }
        if constexpr (Policy == run_policy::once) {
          ecs.template emplace<running<FuncT, run_once_tag>>(std::get<0>(entry));
        }
      }
    } else if constexpr (spawn_policy == run_policy::detached) {
      co_return co_await each_parallel_detached(cfg.main_io_ctx, cfg.concurrent_io_ctx, ecs, dt, cfg.handler, view, run_policy_t<Policy>{});
    }
    co_return true;
  };
}

template <typename ExecutorT, typename FuncT, typename ViewT, run_policy Policy>
asio::awaitable<void> system::each_parallel(ExecutorT& executor, entt_ext::ecs& ecs, double dt, FuncT handler, ViewT& view, run_policy_t<Policy>) {
  using entry_type = decltype(*(view.each().begin()));
  using op_type    = decltype(asio::co_spawn(executor, std::apply(handler, std::tuple_cat(std::tie(ecs, dt), std::declval<entry_type>())), asio::deferred));
  std::vector<op_type> ops;
  for (auto entry : view.each()) {
    ops.push_back(asio::co_spawn(executor, std::apply(handler, std::tuple_cat(std::tie(ecs, dt), entry)), asio::deferred));
    if constexpr (Policy == run_policy::once) {
      ecs.template emplace<running<FuncT, run_once_tag>>(std::get<0>(entry));
    }
  }
  co_await asio::post(executor, asio::use_awaitable);
  if (ops.size() > 0) {
    co_await asio::experimental::make_parallel_group(std::move(ops)).async_wait(asio::experimental::wait_for_all(), asio::use_awaitable);
  }
  co_return;
}

template <typename MainExecutorT, typename ConcurrentExecutorT, typename FuncT, typename ViewT, run_policy Policy>
asio::awaitable<bool> system::each_parallel_detached(MainExecutorT& main_executor, ConcurrentExecutorT& concurrent_executor, entt_ext::ecs& ecs, double dt, FuncT handler, ViewT& view, run_policy_t<Policy>) {

  co_await asio::this_coro::executor;

  for (auto entry : view.each()) {
    auto entity = std::get<0>(entry);

    if (ecs.template any_of<running<FuncT, each_tag>, running<each_tag>>(entity)) {
      continue;
    }

    ecs.template emplace<running<each_tag>>(entity);
    ecs.template emplace<running<FuncT, each_tag>>(entity);
    if constexpr (Policy == run_policy::once) {
      ecs.template emplace<running<FuncT, run_once_tag>>(entity);
    }
    asio::co_spawn(
        concurrent_executor,
        [&main_executor, entity, &ecs, dt, handler, entry]() -> asio::awaitable<void> {
          co_await asio::this_coro::executor;
          co_await std::apply(handler, std::tuple_cat(std::tie(ecs, dt), entry));
          asio::post(main_executor, [&ecs, entity]() {
            ecs.template remove<running<each_tag>>(entity);
            ecs.template remove<running<FuncT, each_tag>>(entity);
          });
          co_return;
        },
        asio::detached);
  }
  co_return true;
}

template <typename ExecutorT, typename FuncT, typename ViewT>
asio::awaitable<void> system::run_parallel(ExecutorT& executor, entt_ext::ecs& ecs, double dt, FuncT handler, ViewT& view) {
  using view_type = decltype(view);
  using op_type   = decltype(asio::co_spawn(executor, std::apply(handler, std::tie(ecs, dt, std::declval<view_type>())), asio::deferred));
  std::vector<op_type> ops;
  ops.push_back(asio::co_spawn(executor, std::apply(handler, std::tie(ecs, dt, view)), asio::deferred));
  co_await asio::post(executor, asio::use_awaitable);
  if (ops.size() > 0) {
    co_await asio::experimental::make_parallel_group(std::move(ops)).async_wait(asio::experimental::wait_for_all(), asio::use_awaitable);
  }
  co_return;
}

///////// SYSTEM BUILDER IMPLEMENTATION /////////
template <typename... ComponentsT, typename... ExcludeT>
system_builder<entt::get_t<ComponentsT...>, entt::exclude_t<ExcludeT...>>::system_builder(entt_ext::ecs& ecs, asio::io_context& main_io_ctx, asio::io_context& concurrent_io_ctx)
  : m_ecs(ecs)
  , m_main_io_context(main_io_ctx)
  , m_concurrent_io_context(concurrent_io_ctx)
  , m_entity(ecs.create()) {
}
template <typename... ComponentsT, typename... ExcludeT>
template <typename FuncT, run_policy Policy>
system_builder<entt::get_t<ComponentsT...>, entt::exclude_t<ExcludeT...>>& system_builder<entt::get_t<ComponentsT...>, entt::exclude_t<ExcludeT...>>::each(FuncT&& func, run_policy_t<Policy>) {
  m_ecs.template emplace<system>(m_entity, typename system::each_config<FuncT, Policy, entt::get_t<ComponentsT...>, entt::exclude_t<ExcludeT...>>{.entity = m_entity, .handler = func, .main_io_ctx = m_main_io_context, .concurrent_io_ctx = m_concurrent_io_context}, m_ecs);

  return *this;
}
template <typename... ComponentsT, typename... ExcludeT>
template <typename FuncT>
system_builder<entt::get_t<ComponentsT...>, entt::exclude_t<ExcludeT...>>& system_builder<entt::get_t<ComponentsT...>, entt::exclude_t<ExcludeT...>>::each_once(FuncT&& func) {
  m_ecs.template emplace<system>(m_entity, typename system::each_config<FuncT, run_policy::once, entt::get_t<ComponentsT...>, entt::exclude_t<ExcludeT...>>{.entity = m_entity, .handler = func, .main_io_ctx = m_main_io_context, .concurrent_io_ctx = m_concurrent_io_context}, m_ecs);

  return *this;
}

// template <typename... ComponentsT, typename... ExcludeT>
// template <typename FuncT, run_policy Policy>
//   requires std::is_invocable_v<FuncT, entt_ext::ecs&, double, typename system_builder<entt::get_t<ComponentsT...>, entt::exclude_t<ExcludeT...>>::ViewT&>
// auto system_builder<entt::get_t<ComponentsT...>, entt::exclude_t<ExcludeT...>>::run(FuncT&& func, run_policy_t<Policy>) -> system_builder& {
//   using builder_type = system_builder<entt::get_t<ComponentsT...>, entt::exclude_t<ExcludeT...>>;
//   using view_type    = typename builder_type::ViewT;
//   using func_type    = std::function<std::invoke_result_t<FuncT, entt_ext::ecs&, double, view_type&>(entt_ext::ecs&, double, view_type&)>;

//   return run_impl(func_type(std::forward<FuncT>(func)), run_policy_t<Policy>{});
// }

template <typename... ComponentsT, typename... ExcludeT>
template <typename FuncT, run_policy Policy, typename>
auto system_builder<entt::get_t<ComponentsT...>, entt::exclude_t<ExcludeT...>>::run(FuncT&& func, run_policy_t<Policy>) -> system_builder& {
  using builder_type = system_builder<entt::get_t<ComponentsT...>, entt::exclude_t<ExcludeT...>>;
  using view_type    = typename builder_type::ViewT;
  using func_type    = std::function<std::invoke_result_t<FuncT, entt_ext::ecs&, double, view_type&>(entt_ext::ecs&, double, view_type&)>;

  return run_impl(func_type(std::forward<FuncT>(func)), run_policy_t<Policy>{});
}

template <typename... ComponentsT, typename... ExcludeT>
template <typename FuncT, typename>
auto system_builder<entt::get_t<ComponentsT...>, entt::exclude_t<ExcludeT...>>::run_once(FuncT&& func) -> system_builder& {
  using ReturnType = std::invoke_result_t<FuncT, entt_ext::ecs&, double, typename system_builder<entt::get_t<ComponentsT...>, entt::exclude_t<ExcludeT...>>::ViewT&>;

  return run_impl(std::function<ReturnType(entt_ext::ecs&, double, typename system_builder<entt::get_t<ComponentsT...>, entt::exclude_t<ExcludeT...>>::ViewT&)>(func), run_policy_t<run_policy::once>{});
}

template <typename... ComponentsT, typename... ExcludeT>
system_builder<entt::get_t<ComponentsT...>, entt::exclude_t<ExcludeT...>>& system_builder<entt::get_t<ComponentsT...>, entt::exclude_t<ExcludeT...>>::interval(double value) {
  m_ecs.template get<system>(m_entity).interval = value;
  return *this;
}

template <typename... ComponentsT, typename... ExcludeT>
system_builder<entt::get_t<ComponentsT...>, entt::exclude_t<ExcludeT...>>& system_builder<entt::get_t<ComponentsT...>, entt::exclude_t<ExcludeT...>>::stage(uint32_t value) {
  m_ecs.template get<system>(m_entity).stage = value;
  return *this;
}

template <typename... ComponentsT, typename... ExcludeT>
template <typename FuncT, run_policy Policy>
system_builder<entt::get_t<ComponentsT...>, entt::exclude_t<ExcludeT...>>& system_builder<entt::get_t<ComponentsT...>, entt::exclude_t<ExcludeT...>>::run_impl(FuncT&& func, run_policy_t<Policy>) {

  m_ecs.template emplace<system>(m_entity, typename system::run_config<FuncT, Policy, entt::get_t<ComponentsT...>, entt::exclude_t<ExcludeT...>>{.entity = m_entity, .handler = func, .main_io_ctx = m_main_io_context, .concurrent_io_ctx = m_concurrent_io_context}, m_ecs);
  return *this;
}

///////// ECS IMPLEMENTATION /////////

template <typename... ComponentsT, typename... ExcludeT>
auto ecs::system(entt::exclude_t<ExcludeT...>) -> system_builder<entt::get_t<ComponentsT...>, entt::exclude_t<ExcludeT...>> {
  return system_builder<entt::get_t<ComponentsT...>, entt::exclude_t<ExcludeT...>>(*this, main_io_context(), concurrent_io_context());
}

template <typename... ComponentsT>
auto ecs::system() -> system_builder<entt::get_t<ComponentsT...>, entt::exclude_t<>> {
  return system<ComponentsT...>(entt::exclude_t<>{});
}

template <typename ArchiveT, typename... ComponentsT>
asio::awaitable<void> ecs::load_snapshot(ArchiveT& ar) {

  auto exectuor = co_await asio::this_coro::executor;
  co_return co_await asio::async_compose<decltype(asio::use_awaitable), void()>(
      [&ar, &exectuor, this](auto&& self) {
        asio::post(exectuor, [self = std::move(self), &ar, &exectuor, this]() mutable {
          {
            entt::snapshot_loader{m_registry}.get<entt_ext::entity>(ar);
            (entt::snapshot_loader{m_registry}.template get<ComponentsT>(ar), ...);
          }
          self.complete();
        });
      },
      asio::use_awaitable);
}

template <typename ArchiveT, typename... ComponentsT>
asio::awaitable<void> ecs::save_snapshot(ArchiveT& ar) const {
  auto exectuor = co_await asio::this_coro::executor;
  co_return co_await asio::async_compose<decltype(asio::use_awaitable), void()>(
      [&ar, &exectuor, this](auto&& self) {
        asio::post(exectuor, [self = std::move(self), &ar, &exectuor, this]() mutable {
          {
            entt::snapshot{m_registry}.get<entt_ext::entity>(ar);
            (entt::snapshot{m_registry}.template get<ComponentsT>(ar), ...);
          }
          self.complete();
        });
      },
      asio::use_awaitable);
}

template <typename ArchiveT, typename... ComponentsT>
asio::awaitable<bool> ecs::merge_snapshot(ArchiveT& ar) {
  auto exectuor = co_await asio::this_coro::executor;
  co_return co_await asio::async_compose<decltype(asio::use_awaitable), void(bool)>(
      [&ar, &exectuor, this](auto&& self) {
        asio::post(exectuor, [self = std::move(self), &ar, &exectuor, this]() mutable {
          {
            m_continuous_loader.get<entt_ext::entity>(ar);
            (m_continuous_loader.template get<ComponentsT>(ar), ...);

            m_continuous_loader.orphans();
            //(details::remap_relationships<ComponentsT>(m_continuous_loader, *this), ...);
          }
          self.complete(true);
        });
      },
      asio::use_awaitable);
}

template <typename Type, typename... Args>
auto ecs::emplace_async(entity_type entt, Args&&... args) -> std::enable_if_t<!std::is_same_v<void, std::decay_t<decltype(std::declval<ecs>().template emplace<Type>(entt, std::forward<Args>(args)...))>>, asio::awaitable<std::reference_wrapper<Type>>> {
  co_return co_await asio::async_compose<decltype(asio::use_awaitable), void(std::reference_wrapper<Type>)>(
      [entt, this, ... args = std::forward<Args>(args)](auto&& self) mutable {
        asio::post(main_io_context(), [self = std::move(self), entt, this, ... args = std::forward<Args>(args)]() mutable {
          self.complete(std::ref(emplace<Type>(entt, std::forward<Args>(args)...)));
        });
      },
      asio::use_awaitable);
}

template <typename Type, typename... Args>
auto ecs::emplace_async(entity_type entt, Args&&... args) -> std::enable_if_t<std::is_same_v<void, std::decay_t<decltype(std::declval<ecs>().template emplace<Type>(entt, std::forward<Args>(args)...))>>, asio::awaitable<void>> {
  co_return co_await asio::async_compose<decltype(asio::use_awaitable), void()>(
      [entt, this, ... args = std::move(args)](auto&& self) {
        asio::post(main_io_context(), [self = std::move(self), entt, this, ... args = move(args)]() mutable {
          emplace<Type>(entt, std::forward<Args>(args)...);
          self.complete();
        });
      },
      asio::use_awaitable);
}

template <typename Type, typename... Args>
auto ecs::emplace_if_not_exists_async(entity_type entt, Args&&... args) -> std::enable_if_t<!std::is_same_v<void, std::decay_t<decltype(std::declval<ecs>().template emplace_if_not_exists<Type>(entt, std::forward<Args>(args)...))>>, asio::awaitable<void>> {
  co_return co_await asio::async_compose<decltype(asio::use_awaitable), void()>(
      [entt, this, ... args = std::forward<Args>(args)](auto&& self) mutable {
        asio::post(main_io_context(), [self = std::move(self), entt, this, ... args = std::forward<Args>(args)]() mutable {
          emplace_if_not_exists<Type>(entt, std::forward<Args>(args)...);
          self.complete();
        });
      },
      asio::use_awaitable);
}

template <typename Type>
auto ecs::get_async(entity_type entt) const -> std::enable_if_t<!std::is_same_v<void, std::decay_t<decltype(std::declval<ecs>().template get<Type>(entt))>>, asio::awaitable<std::reference_wrapper<Type>>> {
  co_return co_await asio::async_compose<decltype(asio::use_awaitable), void(std::reference_wrapper<Type>)>(
      [entt, this](auto&& self) {
        asio::post(const_cast<ecs*>(this)->main_io_context(), [self = std::move(self), entt, this]() mutable {
          self.complete(std::ref(get<Type>(entt)));
        });
      },
      asio::use_awaitable);
}

template <typename Type, typename... Args>
asio::awaitable<size_t> ecs::remove_async(entity_type entt) {
  co_return co_await asio::async_compose<decltype(asio::use_awaitable), void(size_t)>(
      [entt, this](auto&& self) {
        asio::post(main_io_context(), [self = std::move(self), entt, this]() mutable {
          self.complete(remove<Type, Args...>(entt));
        });
      },
      asio::use_awaitable);
}

template <typename Type, typename... Other, typename It>
asio::awaitable<size_t> ecs::remove_async(It first, It last) {
  co_return co_await asio::async_compose<decltype(asio::use_awaitable), void(size_t)>(
      [first, last, this](auto&& self) {
        asio::post(main_io_context(), [self = std::move(self), first, last, this]() mutable {
          self.complete(remove<Type, Other...>(first, last));
        });
      },
      asio::use_awaitable);
}

template <typename Type, typename... Args>
auto ecs::emplace_or_replace_async(entity_type entt, Args... args) -> std::enable_if_t<!std::is_same_v<void, std::decay_t<decltype(std::declval<ecs>().template emplace_or_replace<Type>(entt, std::forward<Args>(args)...))>>, asio::awaitable<std::reference_wrapper<Type>>> {
  co_await asio::this_coro::executor;

  co_return co_await asio::async_compose<decltype(asio::use_awaitable), void(std::reference_wrapper<Type>)>(
      [entt, this, args...](auto&& self) {
        asio::post(main_io_context(), [self = std::move(self), entt, this, args...]() mutable {
          self.complete(std::ref(emplace_or_replace<Type>(entt, args...)));
        });
      },
      asio::use_awaitable);
}

template <typename Type, typename... Args>
auto ecs::emplace_or_replace_async(entity_type entt, Args&&... args) -> std::enable_if_t<std::is_same_v<void, std::decay_t<decltype(std::declval<ecs>().template emplace_or_replace<Type>(entt, std::forward<Args>(args)...))>>, asio::awaitable<void>> {
  co_return co_await asio::async_compose<decltype(asio::use_awaitable), void()>(
      [entt, this, args...](auto&& self) {
        asio::post(main_io_context(), [self = std::move(self), entt, this, args...]() mutable {
          emplace_or_replace<Type>(entt, args...);
          self.complete();
        });
      },
      asio::use_awaitable);
}

#ifndef __EMSCRIPTEN__
template <typename ArchiveT, typename... ComponentsT>
asio::awaitable<void> ecs::load_graph_snapshot(ArchiveT& ar) {

  auto exectuor = co_await asio::this_coro::executor;
  co_return co_await asio::async_compose<decltype(asio::use_awaitable), void()>(
      [&ar, &exectuor, this](auto&& self) {
        asio::post(exectuor, [self = std::move(self), &ar, &exectuor, this]() mutable {
          {
            entt::snapshot_loader{m_registry}.get<entt_ext::entity>(ar).template get<node_descriptor>(ar).template get<pin_descriptor>(ar);
            graph().load_snapshot(ar);
            (entt::snapshot_loader{m_registry}.template get<ComponentsT>(ar), ...);
          }
          self.complete();
        });
      },
      asio::use_awaitable);
}

template <typename ArchiveT, typename... ComponentsT>
asio::awaitable<void> ecs::save_graph_snapshot(ArchiveT& ar) const {
  auto exectuor = co_await asio::this_coro::executor;
  co_return co_await asio::async_compose<decltype(asio::use_awaitable), void()>(
      [&ar, &exectuor, this](auto&& self) {
        asio::post(exectuor, [self = std::move(self), &ar, &exectuor, this]() mutable {
          {
            entt::snapshot{m_registry}.get<entt_ext::entity>(ar).template get<node_descriptor>(ar).template get<pin_descriptor>(ar);
            graph().save_snapshot(ar);
            (entt::snapshot{m_registry}.template get<ComponentsT>(ar), ...);
          }
          self.complete();
        });
      },
      asio::use_awaitable);
}

template <typename ArchiveT, typename... ComponentsT>
asio::awaitable<bool> ecs::merge_graph_snapshot(ArchiveT& ar) {
  auto exectuor = co_await asio::this_coro::executor;
  co_return co_await asio::async_compose<decltype(asio::use_awaitable), void(bool)>(
      [&ar, &exectuor, this](auto&& self) {
        asio::post(exectuor, [self = std::move(self), &ar, &exectuor, this]() mutable {
          {
            m_continuous_loader.get<entt_ext::entity>(ar).template get<node_descriptor>(ar).template get<pin_descriptor>(ar);
            graph().load_snapshot(ar);
            graph().remap(m_continuous_loader);
            (m_continuous_loader.template get<ComponentsT>(ar), ...);

            m_continuous_loader.orphans();
            //(details::remap_relationships<ComponentsT>(m_continuous_loader, *this), ...);
          }
          self.complete(true);
        });
      },
      asio::use_awaitable);
}
#endif

} // namespace entt_ext