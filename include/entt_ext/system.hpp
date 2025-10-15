

struct system {

  using ecs_type = entt_ext::ecs;

  template <typename FuncT, run_policy, typename... ArgsT>
  struct run_config {
    std::function<asio::awaitable<void>(ecs_type&, system&, double)> handler;
  };

  template <typename FuncT, run_policy Policy, typename... ComponentsT, typename... ExcludeT>
  struct run_config<FuncT, Policy, entt::get_t<ComponentsT...>, entt::exclude_t<ExcludeT...>> {
    entt_ext::entity  entity;
    FuncT             handler;
    run_policy        policy;
    asio::io_context& main_io_ctx;
    asio::io_context& concurrent_io_ctx;
  };

  template <typename FuncT, run_policy, typename... ArgsT>
  struct each_config {
    FuncT handler;
  };

  template <typename FuncT, run_policy Policy, typename... ComponentsT, typename... ExcludeT>
  struct each_config<FuncT, Policy, entt::get_t<ComponentsT...>, entt::exclude_t<ExcludeT...>> {
    entt_ext::entity  entity;
    FuncT             handler;
    asio::io_context& main_io_ctx;
    asio::io_context& concurrent_io_ctx;
  };

  template <typename FuncT, run_policy Policy, typename... ComponentsT, typename... ExcludeT>
  system(run_config<FuncT, Policy, entt::get_t<ComponentsT...>, entt::exclude_t<ExcludeT...>>&& cfg, ecs_type& ecs) {

    // using ViewT = decltype(std::declval<entt_ext::ecs>().template view<ComponentsT...>(entt::exclude_t<ExcludeT...>{}));

    run = [cfg  = std::move(cfg),
           view = ecs.template view<ComponentsT...>(entt::exclude_t<ExcludeT...>{}),
           this](entt_ext::system& self, ecs_type& ecs, double dt) mutable -> asio::awaitable<bool> {
      auto executor = co_await asio::this_coro::executor;

      constexpr run_policy spawn_policy =
          (Policy == run_policy::automatic || Policy == run_policy::once)
              ? (std::is_same_v<return_type_t<FuncT>, asio::awaitable<void>> ? run_policy::parallel : run_policy::sequential)
              : Policy;

      if (view.begin() == view.end()) {
        co_return false;
      }

      if constexpr (Policy == run_policy::once) {
        ecs.template emplace<run_once_tag>(cfg.entity);
      }

      if constexpr (spawn_policy == run_policy::parallel && std::is_same_v<return_type_t<FuncT>, asio::awaitable<void>>) {
        co_await run_parallel(cfg.concurrent_io_ctx, ecs, self, dt, cfg.handler, view);
      } else if constexpr (spawn_policy == run_policy::sequential) {
        if constexpr (std::is_same_v<return_type_t<FuncT>, void>) {
          std::apply(cfg.handler, std::tie(ecs, self, dt, view));
        } else if constexpr (std::is_same_v<return_type_t<FuncT>, asio::awaitable<void>>) {
          co_await std::apply(cfg.handler, std::tie(ecs, self, dt, view));
        }
      } else if constexpr (spawn_policy == run_policy::detached) {
        ecs.template emplace<running<system_tag>>(cfg.entity);
        asio::co_spawn(
            cfg.concurrent_io_ctx,
            [entity = cfg.entity, &main_io_ctx = cfg.main_io_ctx, &ecs, &self, dt, handler = cfg.handler, view]() mutable -> asio::awaitable<void> {
              auto executor = co_await asio::this_coro::executor;
              co_await std::apply(handler, std::tie(ecs, self, dt, view));
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
  system(each_config<FuncT, Policy, entt::get_t<ComponentsT...>, entt::exclude_t<ExcludeT...>>&& cfg, ecs_type& ecs) {
    // using ViewT = decltype(std::declval<entt_ext::ecs>().template view<ComponentsT...>(entt::exclude_t<ExcludeT...>{}));

    run = [cfg  = std::move(cfg),
           view = ecs.template view<ComponentsT...>(entt::exclude_t<running<FuncT>, running<FuncT, run_once_tag>, ExcludeT...>{}),
           this](system& self, ecs_type& ecs, double dt) mutable -> asio::awaitable<bool> {
      auto executor = co_await asio::this_coro::executor;

      constexpr run_policy spawn_policy =
          (Policy == run_policy::automatic || Policy == run_policy::once)
              ? (std::is_same_v<return_type_t<FuncT>, asio::awaitable<void>> ? run_policy::parallel : run_policy::sequential)
              : Policy;

      if (view.begin() == view.end()) {
        co_return false;
      }

      if constexpr (spawn_policy == run_policy::parallel && std::is_same_v<return_type_t<FuncT>, asio::awaitable<void>>) {
        co_await each_parallel(cfg.concurrent_io_ctx, ecs, self, dt, cfg.handler, view, run_policy_t<Policy>{});
      } else if constexpr (spawn_policy == run_policy::sequential) {
        for (auto entry : view.each()) {
          if constexpr (std::is_same_v<return_type_t<FuncT>, void>) {
            std::apply(cfg.handler, std::tuple_cat(std::tie(ecs, self, dt), entry));
          } else if constexpr (std::is_same_v<return_type_t<FuncT>, asio::awaitable<void>>) {
            co_await std::apply(cfg.handler, std::tuple_cat(std::tie(ecs, self, dt), entry));
          }
          if constexpr (Policy == run_policy::once) {
            ecs.template emplace<running<FuncT, run_once_tag>>(std::get<0>(entry));
          }
        }
      } else if constexpr (spawn_policy == run_policy::detached) {
        co_return co_await each_parallel_detached(cfg.main_io_ctx, cfg.concurrent_io_ctx, ecs, self, dt, cfg.handler, view, run_policy_t<Policy>{});
      }
      co_return true;
    };
  }

  // Constructor for empty component systems (periodic tasks with no entity queries)
  template <typename FuncT, run_policy Policy>
  system(each_config<FuncT, Policy, void, void>&& cfg, ecs_type& ecs) {
    run = [handler = std::move(cfg.handler)](system& self, ecs_type& ecs, double dt) mutable -> asio::awaitable<bool> {
      if constexpr (std::is_same_v<std::invoke_result_t<FuncT, ecs_type&, system&, double>, asio::awaitable<void>>) {
        co_await handler(ecs, self, dt);
      } else {
        handler(ecs, self, dt);
      }
      co_return true;
    };
  }

  template <typename ExecutorT, typename FuncT, typename ViewT, run_policy Policy>
  static asio::awaitable<void>
  each_parallel(ExecutorT& executor, ecs_type& ecs, system& self, double dt, FuncT handler, ViewT& view, run_policy_t<Policy> = {}) {
    using entry_type = decltype(*(view.each().begin()));
    using op_type =
        decltype(asio::co_spawn(executor, std::apply(handler, std::tuple_cat(std::tie(ecs, self, dt), std::declval<entry_type>())), asio::deferred));
    std::vector<op_type> ops;
    for (auto entry : view.each()) {
      ops.push_back(asio::co_spawn(executor, std::apply(handler, std::tuple_cat(std::tie(ecs, self, dt), entry)), asio::deferred));
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
  static asio::awaitable<bool> each_parallel_detached(MainExecutorT&       main_executor,
                                                      ConcurrentExecutorT& concurrent_executor,
                                                      ecs_type&            ecs,
                                                      system&              self,
                                                      double               dt,
                                                      FuncT                handler,
                                                      ViewT&               view,
                                                      run_policy_t<Policy> = {}) {

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
          [&main_executor, entity, &ecs, &self, dt, handler, entry]() -> asio::awaitable<void> {
            co_await asio::this_coro::executor;
            co_await std::apply(handler, std::tuple_cat(std::tie(ecs, self, dt), entry));
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
  static asio::awaitable<void> run_parallel(ExecutorT& executor, ecs_type& ecs, system& self, double dt, FuncT handler, ViewT& view) {
    using view_type = decltype(view);
    using op_type   = decltype(asio::co_spawn(executor, std::apply(handler, std::tie(ecs, self, dt, std::declval<view_type>())), asio::deferred));
    std::vector<op_type> ops;
    ops.push_back(asio::co_spawn(executor, std::apply(handler, std::tie(ecs, self, dt, view)), asio::deferred));
    co_await asio::post(executor, asio::use_awaitable);
    if (ops.size() > 0) {
      co_await asio::experimental::make_parallel_group(std::move(ops)).async_wait(asio::experimental::wait_for_all(), asio::use_awaitable);
    }
    co_return;
  }

  std::function<asio::awaitable<bool>(system&, entt_ext::ecs&, double)> run;

  time_point last_invoke = {};
  double     interval    = 0.0;
  uint32_t   stage       = stage::render + 1;
};

// Specialization for systems with no components (periodic tasks)
template <>
struct system_builder<entt::get_t<>, entt::exclude_t<>> {
  explicit system_builder(entt_ext::ecs& ecs, asio::io_context& main_io_ctx, asio::io_context& concurrent_io_ctx)
    : m_ecs(ecs)
    , m_main_io_context(main_io_ctx)
    , m_concurrent_io_context(concurrent_io_ctx)
    , m_entity(ecs.create()) {
  }

  template <typename FuncT, run_policy Policy = run_policy::automatic>
  system_builder& run(FuncT&& func, run_policy_t<Policy> = {}) {
    m_ecs.template emplace<system>(m_entity, typename system::each_config<FuncT, Policy, void, void>{.handler = std::forward<FuncT>(func)}, m_ecs);
    return *this;
  }

  system_builder& interval(double value) {
    m_ecs.template get<system>(m_entity).interval = value;
    return *this;
  }

  system_builder& stage(uint32_t value) {
    m_ecs.template get<system>(m_entity).stage = value;
    return *this;
  }

private:
  entt_ext::ecs&    m_ecs;
  asio::io_context& m_main_io_context;
  asio::io_context& m_concurrent_io_context;
  entt_ext::entity  m_entity;
};

template <typename... ComponentsT, typename... ExcludeT>
struct system_builder<entt::get_t<ComponentsT...>, entt::exclude_t<ExcludeT...>> {
  using ViewT = decltype(std::declval<entt_ext::ecs>().template view<ComponentsT...>(entt::exclude_t<ExcludeT...>{}));

  explicit system_builder(entt_ext::ecs& ecs, asio::io_context& main_io_ctx, asio::io_context& concurrent_io_ctx)
    : m_ecs(ecs)
    , m_main_io_context(main_io_ctx)
    , m_concurrent_io_context(concurrent_io_ctx)
    , m_entity(ecs.create()) {
  }

  template <typename FuncT, run_policy Policy = run_policy::automatic>
  system_builder& each(FuncT&& func, run_policy_t<Policy> = {}) {
    m_ecs.template emplace<system>(m_entity,
                                   typename system::each_config<FuncT, Policy, entt::get_t<ComponentsT...>, entt::exclude_t<ExcludeT...>>{
                                       .entity            = m_entity,
                                       .handler           = func,
                                       .main_io_ctx       = m_main_io_context,
                                       .concurrent_io_ctx = m_concurrent_io_context},
                                   m_ecs);

    return *this;
  }

  template <typename FuncT>
  system_builder& each_once(FuncT&& func) {
    m_ecs.template emplace<system>(m_entity,
                                   typename system::each_config<FuncT, run_policy::once, entt::get_t<ComponentsT...>, entt::exclude_t<ExcludeT...>>{
                                       .entity            = m_entity,
                                       .handler           = func,
                                       .main_io_ctx       = m_main_io_context,
                                       .concurrent_io_ctx = m_concurrent_io_context},
                                   m_ecs);

    return *this;
  }

  // Modified run function using SFINAE
  template <typename FuncT,
            run_policy Policy = run_policy::automatic,
            typename          = std::enable_if_t<std::is_invocable_v<FuncT, entt_ext::ecs&, system&, double, ViewT&>>>
  auto run(FuncT&& func, run_policy_t<Policy> = {}) -> system_builder& {
    using builder_type = system_builder<entt::get_t<ComponentsT...>, entt::exclude_t<ExcludeT...>>;
    using view_type    = typename builder_type::ViewT;
    using func_type =
        std::function<std::invoke_result_t<FuncT, entt_ext::ecs&, system&, double, view_type&>(entt_ext::ecs&, system&, double, view_type&)>;

    return run_impl(func_type(std::forward<FuncT>(func)), run_policy_t<Policy>{});
  }

  // Modified run_once function using SFINAE
  template <typename FuncT, typename = std::enable_if_t<std::is_invocable_v<FuncT, entt_ext::ecs&, double, ViewT&>>>
  auto run_once(FuncT&& func) -> system_builder& {
    using ReturnType = std::invoke_result_t<FuncT,
                                            entt_ext::ecs&,
                                            system&,
                                            double,
                                            typename system_builder<entt::get_t<ComponentsT...>, entt::exclude_t<ExcludeT...>>::ViewT&>;

    return run_impl(
        std::function<
            ReturnType(entt_ext::ecs&, system&, double, typename system_builder<entt::get_t<ComponentsT...>, entt::exclude_t<ExcludeT...>>::ViewT&)>(
            func),
        run_policy_t<run_policy::once>{});
  }

  system_builder& interval(double value) {
    m_ecs.template get<system>(m_entity).interval = value;
    return *this;
  }

  system_builder& stage(uint32_t value) {
    m_ecs.template get<system>(m_entity).stage = value;
    return *this;
  }

private:
  template <typename FuncT, run_policy Policy>
  system_builder& run_impl(FuncT&& func, run_policy_t<Policy> = {}) {

    m_ecs.template emplace<system>(m_entity,
                                   typename system::run_config<FuncT, Policy, entt::get_t<ComponentsT...>, entt::exclude_t<ExcludeT...>>{
                                       .entity            = m_entity,
                                       .handler           = func,
                                       .main_io_ctx       = m_main_io_context,
                                       .concurrent_io_ctx = m_concurrent_io_context},
                                   m_ecs);
    return *this;
  }

private:
  entt_ext::ecs&    m_ecs;
  asio::io_context& m_main_io_context;
  asio::io_context& m_concurrent_io_context;
  entt_ext::entity  m_entity;
};
