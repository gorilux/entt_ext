// Type trait to detect children_view
template <typename T>
struct is_children_view : std::false_type {};

template <typename Type, typename... Others>
struct is_children_view<children_view<Type, Others...>> : std::true_type {};

template <typename T>
inline constexpr bool is_children_view_v = is_children_view<T>::value;

// Type trait to detect optional_tag
template <typename T>
struct is_optional_tag : std::false_type {};

template <typename T>
struct is_optional_tag<optional_tag<T>> : std::true_type {};

template <typename T>
inline constexpr bool is_optional_tag_v = is_optional_tag<T>::value;

// Unwrap optional_tag<T> -> T
template <typename T>
struct unwrap_optional_tag;

template <typename T>
struct unwrap_optional_tag<optional_tag<T>> {
  using type = T;
};

// A marker is any template parameter that is NOT a required component:
// currently {children_view<...>, optional_tag<...>}. Markers are stripped
// from the view query and replaced at call time with computed arguments.
template <typename T>
inline constexpr bool is_marker_v = is_children_view_v<T> || is_optional_tag_v<T>;

// Helper to filter children_view types from component list
template <typename... Ts>
struct filter_children_views;

template <>
struct filter_children_views<> {
  using type = std::tuple<>;
};

template <typename T, typename... Rest>
struct filter_children_views<T, Rest...> {
  using rest_type = typename filter_children_views<Rest...>::type;
  using type =
      std::conditional_t<is_children_view_v<T>, rest_type, decltype(std::tuple_cat(std::declval<std::tuple<T>>(), std::declval<rest_type>()))>;
};

// Helper to extract only children_view types
template <typename... Ts>
struct extract_children_views;

template <>
struct extract_children_views<> {
  using type = std::tuple<>;
};

template <typename T, typename... Rest>
struct extract_children_views<T, Rest...> {
  using rest_type = typename extract_children_views<Rest...>::type;
  using type =
      std::conditional_t<is_children_view_v<T>, decltype(std::tuple_cat(std::declval<std::tuple<T>>(), std::declval<rest_type>())), rest_type>;
};

// Filter: keep only regular (non-marker) components. Used to build the view query.
template <typename... Ts>
struct filter_markers;

template <>
struct filter_markers<> {
  using type = std::tuple<>;
};

template <typename T, typename... Rest>
struct filter_markers<T, Rest...> {
  using rest_type = typename filter_markers<Rest...>::type;
  using type =
      std::conditional_t<is_marker_v<T>, rest_type, decltype(std::tuple_cat(std::declval<std::tuple<T>>(), std::declval<rest_type>()))>;
};

// Convert tuple to entt::get_t
template <typename TupleT>
struct tuple_to_get_t;

template <typename... Ts>
struct tuple_to_get_t<std::tuple<Ts...>> {
  using type = entt::get_t<Ts...>;
};

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

  // Helper: Compute spawn policy at compile time
  template <run_policy Policy, typename FuncT>
  static constexpr run_policy compute_spawn_policy() {
    if constexpr (Policy == run_policy::automatic || Policy == run_policy::once) {
      return std::is_same_v<return_type_t<FuncT>, asio::awaitable<void>> ? run_policy::parallel : run_policy::sequential;
    } else {
      return Policy;
    }
  }

  template <typename FuncT, run_policy Policy, typename... ComponentsT, typename... ExcludeT>
  system(run_config<FuncT, Policy, entt::get_t<ComponentsT...>, entt::exclude_t<ExcludeT...>>&& cfg, ecs_type& ecs) {
    // Compute all traits at this level, ONCE
    constexpr run_policy spawn_policy = compute_spawn_policy<Policy, FuncT>();
    constexpr bool       is_async     = std::is_same_v<return_type_t<FuncT>, asio::awaitable<void>>;
    constexpr bool       is_once      = (Policy == run_policy::once);

    // Create specialized runner based on computed policies
    if constexpr (spawn_policy == run_policy::parallel && is_async) {
      run = create_run_parallel_async<FuncT, is_once>(std::move(cfg));
    } else if constexpr (spawn_policy == run_policy::sequential && is_async) {
      run = create_run_sequential_async<FuncT, is_once>(std::move(cfg));
    } else if constexpr (spawn_policy == run_policy::sequential && !is_async) {
      run = create_run_sequential_sync<FuncT, is_once>(std::move(cfg));
    } else if constexpr (spawn_policy == run_policy::detached) {
      run = create_run_detached<FuncT, is_once>(std::move(cfg));
    } else {
      static_assert(spawn_policy == run_policy::parallel || spawn_policy == run_policy::sequential || spawn_policy == run_policy::detached,
                    "Invalid run_policy");
    }
  }

  template <typename FuncT, run_policy Policy, typename... ComponentsT, typename... ExcludeT>
  system(each_config<FuncT, Policy, entt::get_t<ComponentsT...>, entt::exclude_t<ExcludeT...>>&& cfg, ecs_type& ecs) {
    // Compute all traits at this level, ONCE
    constexpr run_policy spawn_policy = compute_spawn_policy<Policy, FuncT>();
    constexpr bool       is_async     = std::is_same_v<return_type_t<FuncT>, asio::awaitable<void>>;
    constexpr bool       is_once      = (Policy == run_policy::once);

    // Create specialized runner based on computed policies
    if constexpr (spawn_policy == run_policy::parallel && is_async) {
      run = create_each_parallel_async<FuncT, Policy, is_once>(std::move(cfg));
    } else if constexpr (spawn_policy == run_policy::sequential && is_async) {
      run = create_each_sequential_async<FuncT, Policy, is_once>(std::move(cfg));
    } else if constexpr (spawn_policy == run_policy::sequential && !is_async) {
      run = create_each_sequential_sync<FuncT, Policy, is_once>(std::move(cfg));
    } else if constexpr (spawn_policy == run_policy::detached) {
      run = create_each_detached<FuncT, Policy, is_once>(std::move(cfg));
    } else {
      static_assert(spawn_policy == run_policy::parallel || spawn_policy == run_policy::sequential || spawn_policy == run_policy::detached,
                    "Invalid run_policy");
    }
  }

  // Constructor for empty component systems (periodic tasks with no entity queries)
  template <typename FuncT, run_policy Policy>
  system(each_config<FuncT, Policy, void, void>&& cfg, ecs_type& ecs) {
    constexpr bool is_async = std::is_same_v<std::invoke_result_t<FuncT, ecs_type&, system&, double>, asio::awaitable<void>>;

    if constexpr (is_async) {
      run = [handler = std::move(cfg.handler)](system& self, ecs_type& ecs, double dt) mutable -> asio::awaitable<bool> {
        co_await handler(ecs, self, dt);
        co_return true;
      };
    } else {
      run = [handler = std::move(cfg.handler)](system& self, ecs_type& ecs, double dt) mutable -> asio::awaitable<bool> {
        handler(ecs, self, dt);
        co_return true;
      };
    }
  }

private:
  // Helper methods to create specialized runners - NO TEMPLATE LOGIC IN LAMBDAS

  template <typename FuncT, bool IsOnce, typename... ComponentsT, typename... ExcludeT>
  static auto create_run_parallel_async(run_config<FuncT, run_policy::parallel, entt::get_t<ComponentsT...>, entt::exclude_t<ExcludeT...>>&& cfg) {
    return [cfg = std::move(cfg)](system& self, ecs_type& ecs, double dt) mutable -> asio::awaitable<bool> {
      auto view = ecs.template view<ComponentsT...>(entt::exclude_t<delete_later, ExcludeT...>{});

      if (view.begin() == view.end()) {
        co_return false;
      }

      if constexpr (IsOnce) {
        ecs.template emplace<run_once_tag>(cfg.entity);
      }

      co_await run_parallel(cfg.concurrent_io_ctx, ecs, self, dt, cfg.handler, view);
      co_return true;
    };
  }

  template <typename FuncT, bool IsOnce, typename... ComponentsT, typename... ExcludeT>
  static auto
  create_run_sequential_async(run_config<FuncT, run_policy::sequential, entt::get_t<ComponentsT...>, entt::exclude_t<ExcludeT...>>&& cfg) {
    return [cfg = std::move(cfg)](system& self, ecs_type& ecs, double dt) mutable -> asio::awaitable<bool> {
      auto view = ecs.template view<ComponentsT...>(entt::exclude_t<delete_later, ExcludeT...>{});

      if (view.begin() == view.end()) {
        co_return false;
      }

      if constexpr (IsOnce) {
        ecs.template emplace<run_once_tag>(cfg.entity);
      }

      co_await cfg.handler(ecs, self, dt, view);
      co_return true;
    };
  }

  template <typename FuncT, bool IsOnce, typename... ComponentsT, typename... ExcludeT>
  static auto create_run_sequential_sync(run_config<FuncT, run_policy::sequential, entt::get_t<ComponentsT...>, entt::exclude_t<ExcludeT...>>&& cfg) {
    return [cfg = std::move(cfg)](system& self, ecs_type& ecs, double dt) mutable -> asio::awaitable<bool> {
      auto view = ecs.template view<ComponentsT...>(entt::exclude_t<delete_later, ExcludeT...>{});

      if (view.begin() == view.end()) {
        co_return false;
      }

      if constexpr (IsOnce) {
        ecs.template emplace<run_once_tag>(cfg.entity);
      }

      cfg.handler(ecs, self, dt, view);
      co_return true;
    };
  }

  template <typename FuncT, bool IsOnce, typename... ComponentsT, typename... ExcludeT>
  static auto create_run_detached(run_config<FuncT, run_policy::detached, entt::get_t<ComponentsT...>, entt::exclude_t<ExcludeT...>>&& cfg) {
    return [cfg = std::move(cfg)](system& self, ecs_type& ecs, double dt) mutable -> asio::awaitable<bool> {
      auto view = ecs.template view<ComponentsT...>(entt::exclude_t<delete_later, ExcludeT...>{});

      if (view.begin() == view.end()) {
        co_return false;
      }

      if constexpr (IsOnce) {
        ecs.template emplace<run_once_tag>(cfg.entity);
      }

      ecs.template emplace<running<system_tag>>(cfg.entity);
      asio::co_spawn(
          cfg.concurrent_io_ctx,
          [entity = cfg.entity, &ecs, &self, dt, handler = cfg.handler, view]() mutable -> asio::awaitable<void> {
            try {
              co_await handler(ecs, self, dt, view);
            } catch (std::exception const& ex) {
              // Running tag is still removed below, but the handler died mid-body —
              // any manual guard state it holds (atomics, flags) may be stuck.
              spdlog::error("entt_ext: detached run handler '{}' threw: {}", entt::type_id<FuncT>().name(), ex.what());
            } catch (...) {
              spdlog::error("entt_ext: detached run handler '{}' threw a non-std exception", entt::type_id<FuncT>().name());
            }
            co_await ecs.remove_deferred<running<system_tag>>(entity);
          },
          asio::detached);
      co_return true;
    };
  }

  template <typename FuncT, run_policy Policy, bool IsOnce, typename... ComponentsT, typename... ExcludeT>
  static auto create_each_parallel_async(each_config<FuncT, Policy, entt::get_t<ComponentsT...>, entt::exclude_t<ExcludeT...>>&& cfg) {
    // Any marker (children_view or optional_tag) triggers the unified path.
    constexpr bool has_markers = (is_marker_v<ComponentsT> || ...);

    if constexpr (has_markers) {
      using regular_components = typename filter_markers<ComponentsT...>::type;

      return create_each_parallel_async_impl_with_markers<FuncT, Policy, IsOnce, ComponentsT...>(std::move(cfg),
                                                                                                 regular_components{},
                                                                                                 entt::exclude_t<ExcludeT...>{});
    } else {
      return [cfg = std::move(cfg)](system& self, ecs_type& ecs, double dt) mutable -> asio::awaitable<bool> {
        auto view = ecs.template view<ComponentsT...>(entt::exclude_t<running<FuncT>, delete_later, running<FuncT, run_once_tag>, ExcludeT...>{});

        if (view.begin() == view.end()) {
          co_return false;
        }

        co_await each_parallel(cfg.concurrent_io_ctx, ecs, self, dt, cfg.handler, view, run_policy_t<Policy>{});
        co_return true;
      };
    }
  }

  template <typename FuncT, run_policy Policy, bool IsOnce, typename... ComponentsT, typename... ExcludeT>
  static auto create_each_sequential_async(each_config<FuncT, Policy, entt::get_t<ComponentsT...>, entt::exclude_t<ExcludeT...>>&& cfg) {
    // Any marker (children_view or optional_tag) triggers the unified path.
    constexpr bool has_markers = (is_marker_v<ComponentsT> || ...);

    if constexpr (has_markers) {
      using regular_components = typename filter_markers<ComponentsT...>::type;

      return create_each_sequential_async_impl_with_markers<FuncT, Policy, IsOnce, ComponentsT...>(std::move(cfg),
                                                                                                   regular_components{},
                                                                                                   entt::exclude_t<ExcludeT...>{});
    } else {
      return [cfg = std::move(cfg)](system& self, ecs_type& ecs, double dt) mutable -> asio::awaitable<bool> {
        auto view = ecs.template view<ComponentsT...>(entt::exclude_t<running<FuncT>, delete_later, running<FuncT, run_once_tag>, ExcludeT...>{});

        if (view.begin() == view.end()) {
          co_return false;
        }

        for (auto entry : view.each()) {
          co_await std::apply(cfg.handler, std::tuple_cat(std::tie(ecs, self, dt), entry));

          if constexpr (IsOnce) {
            ecs.template emplace<running<FuncT, run_once_tag>>(std::get<0>(entry));
          }
        }
        co_return true;
      };
    }
  }

  template <typename FuncT, run_policy Policy, bool IsOnce, typename... ComponentsT, typename... ExcludeT>
  static auto create_each_sequential_sync(each_config<FuncT, Policy, entt::get_t<ComponentsT...>, entt::exclude_t<ExcludeT...>>&& cfg) {
    // Any marker (children_view or optional_tag) triggers the unified path.
    constexpr bool has_markers = (is_marker_v<ComponentsT> || ...);

    if constexpr (has_markers) {
      using regular_components = typename filter_markers<ComponentsT...>::type;

      return create_each_sequential_sync_impl_with_markers<FuncT, Policy, IsOnce, ComponentsT...>(std::move(cfg),
                                                                                                  regular_components{},
                                                                                                  entt::exclude_t<ExcludeT...>{});
    } else {
      return [cfg = std::move(cfg)](system& self, ecs_type& ecs, double dt) mutable -> asio::awaitable<bool> {
        auto view = ecs.template view<ComponentsT...>(entt::exclude_t<running<FuncT>, delete_later, running<FuncT, run_once_tag>, ExcludeT...>{});

        if (view.begin() == view.end()) {
          co_return false;
        }

        for (auto entry : view.each()) {
          std::apply(
              [&](auto&&... args) {
                cfg.handler(ecs, self, dt, std::forward<decltype(args)>(args)...);
              },
              entry);
          if constexpr (IsOnce) {
            ecs.template emplace<running<FuncT, run_once_tag>>(std::get<0>(entry));
          }
        }
        co_return true;
      };
    }
  }

  template <typename FuncT, run_policy Policy, bool IsOnce, typename... ComponentsT, typename... ExcludeT>
  static auto create_each_detached(each_config<FuncT, Policy, entt::get_t<ComponentsT...>, entt::exclude_t<ExcludeT...>>&& cfg) {
    // Any marker (children_view or optional_tag) triggers the unified path.
    constexpr bool has_markers = (is_marker_v<ComponentsT> || ...);

    if constexpr (has_markers) {
      using regular_components = typename filter_markers<ComponentsT...>::type;

      return create_each_detached_impl_with_markers<FuncT, Policy, IsOnce, ComponentsT...>(std::move(cfg),
                                                                                           static_cast<regular_components*>(nullptr),
                                                                                           entt::exclude_t<ExcludeT...>{});
    } else {
      return [cfg = std::move(cfg)](system& self, ecs_type& ecs, double dt) mutable -> asio::awaitable<bool> {
        auto view = ecs.template view<ComponentsT...>(entt::exclude_t<running<FuncT>, delete_later, running<FuncT, run_once_tag>, ExcludeT...>{});

        if (view.begin() == view.end()) {
          co_return false;
        }

        co_return co_await each_parallel_detached(cfg.main_io_ctx, cfg.concurrent_io_ctx, ecs, self, dt, cfg.handler, view, run_policy_t<Policy>{});
      };
    }
  }

  // ==========================================================================
  // Unified _impl_with_markers helpers
  //
  // "Markers" are template parameters in ComponentsT... that are NOT required
  // components for the view query: currently children_view<...> and
  // optional_tag<...>. RegularComponents is the subset of ComponentsT used to
  // build the view. At invocation time, we rebuild the handler argument list
  // in the user's declared order (ComponentsT...) — each position is either
  // a reference to a regular component, a nullable pointer from try_get, or a
  // children_range built on-the-fly.
  //
  // Handler signature: (ecs, self, dt, entity, handler_arg_t<ComponentsT>...)
  // ==========================================================================

  // Detached variant: spawn one coroutine per entity on the concurrent executor.
  template <typename FuncT, run_policy Policy, bool IsOnce, typename... ComponentsT, typename... RegularComponents, typename... ExcludeT>
  static auto create_each_detached_impl_with_markers(each_config<FuncT, Policy, entt::get_t<ComponentsT...>, entt::exclude_t<ExcludeT...>>&& cfg,
                                                     std::tuple<RegularComponents...>*,
                                                     entt::exclude_t<ExcludeT...>) {

    return [cfg = std::move(cfg)](system& self, ecs_type& ecs, double dt) mutable -> asio::awaitable<bool> {
      auto view = ecs.template view<RegularComponents...>(entt::exclude_t<running<FuncT>, delete_later, running<FuncT, run_once_tag>, ExcludeT...>{});

      if (view.begin() == view.end()) {
        co_return false;
      }

      co_await asio::this_coro::executor;

      for (auto entity : view) {
        if (!ecs.valid(entity) || ecs.template any_of<running<FuncT, each_tag>>(entity)) {
          continue;
        }

        // Build the args tuple in declared order. children_range elements
        // (if any) are held by value in this tuple.
        auto args = build_args_tuple<ComponentsT...>(ecs, entity);

        // Skip if any children_range argument is empty (preserves prior semantics).
        if (has_empty_children_range_in_args<ComponentsT...>(args)) {
          continue;
        }

        ecs.template emplace<running<FuncT, each_tag>>(entity);
        if constexpr (IsOnce) {
          ecs.template emplace<running<FuncT, run_once_tag>>(entity);
        }
        ecs.detached_each_acquire();

        asio::co_spawn(
            cfg.concurrent_io_ctx,
            [&ecs, entity, &self, dt, handler = cfg.handler, args = std::move(args)]() mutable -> asio::awaitable<void> {
              co_await asio::this_coro::executor;

              try {
                co_await invoke_handler_async(handler, ecs, self, dt, entity, args);
              } catch (std::exception const& ex) {
                // Running tag is still removed below, but the handler died mid-body —
                // any manual guard state it holds (atomics, flags) may be stuck.
                spdlog::error("entt_ext: detached each handler '{}' threw on entity {}: {}", entt::type_id<FuncT>().name(), static_cast<std::uint32_t>(entity), ex.what());
              } catch (...) {
                spdlog::error("entt_ext: detached each handler '{}' threw a non-std exception on entity {}", entt::type_id<FuncT>().name(), static_cast<std::uint32_t>(entity));
              }

              co_await ecs.remove_deferred<running<FuncT, each_tag>>(entity);
              co_await ecs.detached_each_release_deferred();
              co_return;
            },
            asio::detached);
      }
      co_return true;
    };
  }

  // Parallel async variant: spawn all handler coroutines on the concurrent
  // executor and wait for all to complete.
  template <typename FuncT, run_policy Policy, bool IsOnce, typename... ComponentsT, typename... RegularComponents, typename... ExcludeT>
  static auto create_each_parallel_async_impl_with_markers(each_config<FuncT, Policy, entt::get_t<ComponentsT...>, entt::exclude_t<ExcludeT...>>&& cfg,
                                                           std::tuple<RegularComponents...>,
                                                           entt::exclude_t<ExcludeT...>) {

    return [cfg = std::move(cfg)](system& self, ecs_type& ecs, double dt) mutable -> asio::awaitable<bool> {
      auto view = ecs.template view<RegularComponents...>(entt::exclude_t<running<FuncT>, delete_later, running<FuncT, run_once_tag>, ExcludeT...>{});

      if (view.begin() == view.end()) {
        co_return false;
      }

      // Collect all operations with their args.
      std::vector<std::function<asio::awaitable<void>()>> ops;

      for (auto entity : view) {
        auto args = build_args_tuple<ComponentsT...>(ecs, entity);

        if (has_empty_children_range_in_args<ComponentsT...>(args)) {
          continue;
        }

        ops.push_back([&ecs, &self, dt, handler = cfg.handler, entity, args = std::move(args)]() mutable -> asio::awaitable<void> {
          co_await invoke_handler_async(handler, ecs, self, dt, entity, args);
        });

        if constexpr (IsOnce) {
          ecs.template emplace<running<FuncT, run_once_tag>>(entity);
        }
      }

      // Execute all operations in parallel
      if (!ops.empty()) {
        std::vector<decltype(asio::co_spawn(cfg.concurrent_io_ctx, ops[0](), asio::deferred))> spawned_ops;
        for (auto& op : ops) {
          spawned_ops.push_back(asio::co_spawn(cfg.concurrent_io_ctx, op(), asio::deferred));
        }
        co_await asio::experimental::make_parallel_group(std::move(spawned_ops)).async_wait(asio::experimental::wait_for_all(), asio::use_awaitable);
      }

      co_return true;
    };
  }

  // Sequential async variant: await each handler in turn on the main executor.
  template <typename FuncT, run_policy Policy, bool IsOnce, typename... ComponentsT, typename... RegularComponents, typename... ExcludeT>
  static auto create_each_sequential_async_impl_with_markers(each_config<FuncT, Policy, entt::get_t<ComponentsT...>, entt::exclude_t<ExcludeT...>>&& cfg,
                                                             std::tuple<RegularComponents...>,
                                                             entt::exclude_t<ExcludeT...>) {

    return [cfg = std::move(cfg)](system& self, ecs_type& ecs, double dt) mutable -> asio::awaitable<bool> {
      auto view = ecs.template view<RegularComponents...>(entt::exclude_t<running<FuncT>, delete_later, running<FuncT, run_once_tag>, ExcludeT...>{});

      if (view.begin() == view.end()) {
        co_return false;
      }

      for (auto entity : view) {
        auto args = build_args_tuple<ComponentsT...>(ecs, entity);

        if (has_empty_children_range_in_args<ComponentsT...>(args)) {
          continue;
        }

        co_await invoke_handler_async(cfg.handler, ecs, self, dt, entity, args);

        if constexpr (IsOnce) {
          ecs.template emplace<running<FuncT, run_once_tag>>(entity);
        }
      }
      co_return true;
    };
  }

  // Sequential sync variant: call handler directly.
  template <typename FuncT, run_policy Policy, bool IsOnce, typename... ComponentsT, typename... RegularComponents, typename... ExcludeT>
  static auto create_each_sequential_sync_impl_with_markers(each_config<FuncT, Policy, entt::get_t<ComponentsT...>, entt::exclude_t<ExcludeT...>>&& cfg,
                                                            std::tuple<RegularComponents...>,
                                                            entt::exclude_t<ExcludeT...>) {

    return [cfg = std::move(cfg)](system& self, ecs_type& ecs, double dt) mutable -> asio::awaitable<bool> {
      auto view = ecs.template view<RegularComponents...>(entt::exclude_t<running<FuncT>, delete_later, running<FuncT, run_once_tag>, ExcludeT...>{});

      if (view.begin() == view.end()) {
        co_return false;
      }

      for (auto entity : view) {
        auto args = build_args_tuple<ComponentsT...>(ecs, entity);

        if (has_empty_children_range_in_args<ComponentsT...>(args)) {
          continue;
        }

        invoke_handler_sync(cfg.handler, ecs, self, dt, entity, args);

        if constexpr (IsOnce) {
          ecs.template emplace<running<FuncT, run_once_tag>>(entity);
        }
      }
      co_return true;
    };
  }

  // Helper to extract Type and Others from children_view<Type, Others...>
  template <typename ChildViewType>
  struct unpack_children_view;

  template <typename Type, typename... Others>
  struct unpack_children_view<children_view<Type, Others...>> {
    static auto create(ecs_type& ecs, entt_ext::entity entity) -> children_range<ecs_type, Type, Others...> {
      return ecs.template children_view<Type, Others...>(entity);
    }
  };

  // Helper to create a children_range from a children_view marker type
  template <typename ChildViewType>
  static auto create_children_range(ecs_type& ecs, entt_ext::entity entity) {
    return unpack_children_view<ChildViewType>::create(ecs, entity);
  }

  // Helper to check if any children_range in a tuple is empty
  template <typename... ChildRanges>
  static bool any_children_range_empty(ChildRanges&... ranges) {
    return ((!(ranges.each().begin() != ranges.each().end())) || ...);
  }

  // Compute the handler-argument type for each declared template parameter:
  //   regular component T      -> T&
  //   optional_tag<T>          -> T*   (nullable, via ecs.try_get<T>)
  //   children_view<Type,...>  -> children_range<ecs_type, Type, Others...> (by value)
  template <typename T>
  struct handler_arg {
    using type = T&;
  };

  template <typename T>
  struct handler_arg<optional_tag<T>> {
    using type = T*;
  };

  template <typename Type, typename... Others>
  struct handler_arg<children_view<Type, Others...>> {
    using type = children_range<ecs_type, Type, Others...>;
  };

  template <typename T>
  using handler_arg_t = typename handler_arg<T>::type;

  // Produce the handler argument for a single declared template parameter.
  template <typename T>
  static decltype(auto) make_handler_arg(ecs_type& ecs, entt_ext::entity entity) {
    if constexpr (is_optional_tag_v<T>) {
      using inner = typename unwrap_optional_tag<T>::type;
      return ecs.template try_get<inner>(entity); // inner*
    } else if constexpr (is_children_view_v<T>) {
      return create_children_range<T>(ecs, entity); // children_range by value
    } else {
      return ecs.template get<T>(entity); // T&
    }
  }

  // Build a tuple<handler_arg_t<ComponentsT>...> in the user's declared order.
  template <typename... ComponentsT>
  static auto build_args_tuple(ecs_type& ecs, entt_ext::entity entity) {
    return std::tuple<handler_arg_t<ComponentsT>...>{make_handler_arg<ComponentsT>(ecs, entity)...};
  }

  // Check a single tuple element: if its declared type is children_view<...>,
  // return true when its range is empty; otherwise return false. Using
  // `if constexpr` ensures `.each()` is only instantiated for children_range
  // elements — regular component refs and optional pointers are skipped.
  template <typename T, std::size_t I, typename Tuple>
  static bool check_empty_children_at(Tuple& args) {
    if constexpr (is_children_view_v<T>) {
      return !(std::get<I>(args).each().begin() != std::get<I>(args).each().end());
    } else {
      (void)args;
      return false;
    }
  }

  // Scan the args tuple and return true if any element that came from a
  // children_view<...> declaration is empty. Preserves the "skip if any
  // children_range is empty" semantics of the previous _impl_with_children path.
  template <typename... ComponentsT, typename Tuple, std::size_t... Is>
  static bool has_empty_children_range_in_args_impl(Tuple& args, std::index_sequence<Is...>) {
    return (check_empty_children_at<ComponentsT, Is>(args) || ...);
  }

  template <typename... ComponentsT, typename Tuple>
  static bool has_empty_children_range_in_args(Tuple& args) {
    return has_empty_children_range_in_args_impl<ComponentsT...>(args, std::index_sequence_for<ComponentsT...>{});
  }

  // Invoke the handler with (ecs, self, dt, entity, args...) where args is a
  // tuple<handler_arg_t<ComponentsT>...> built in declared order.
  template <typename FuncT, typename Tuple>
  static void invoke_handler_sync(FuncT& handler, ecs_type& ecs, system& self, double dt, entt_ext::entity entity, Tuple& args) {
    std::apply(
        [&](auto&&... a) {
          handler(ecs, self, dt, entity, std::forward<decltype(a)>(a)...);
        },
        args);
  }

  template <typename FuncT, typename Tuple>
  static asio::awaitable<void> invoke_handler_async(FuncT& handler, ecs_type& ecs, system& self, double dt, entt_ext::entity entity, Tuple& args) {
    co_await std::apply(
        [&](auto&&... a) -> asio::awaitable<void> {
          co_return co_await handler(ecs, self, dt, entity, std::forward<decltype(a)>(a)...);
        },
        args);
  }

public:
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

      if (!ecs.valid(entity) || ecs.template any_of<running<FuncT, each_tag>>(entity)) {
        continue;
      }

      ecs.template emplace<running<FuncT, each_tag>>(entity);
      if constexpr (Policy == run_policy::once) {
        ecs.template emplace<running<FuncT, run_once_tag>>(entity);
      }
      ecs.detached_each_acquire();
      asio::co_spawn(
          concurrent_executor,
          [&main_executor, entity, &ecs, &self, dt, handler, entry]() -> asio::awaitable<void> {
            co_await asio::this_coro::executor;
            try {
              co_await std::apply(handler, std::tuple_cat(std::tie(ecs, self, dt), entry));
            } catch (std::exception const& ex) {
              // Running tag is still removed below, but the handler died mid-body —
              // any manual guard state it holds (atomics, flags) may be stuck.
              spdlog::error("entt_ext: detached each handler '{}' threw on entity {}: {}", entt::type_id<FuncT>().name(), static_cast<std::uint32_t>(entity), ex.what());
            } catch (...) {
              spdlog::error("entt_ext: detached each handler '{}' threw a non-std exception on entity {}", entt::type_id<FuncT>().name(), static_cast<std::uint32_t>(entity));
            }

            co_await ecs.remove_deferred<running<FuncT, each_tag>>(entity);
            co_await ecs.detached_each_release_deferred();
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
    : ecs_(ecs)
    , main_io_context_(main_io_ctx)
    , concurrent_io_context_(concurrent_io_ctx)
    , entity_(ecs.create()) {
  }

  template <typename FuncT, run_policy Policy = run_policy::automatic>
  system_builder& run(FuncT&& func, run_policy_t<Policy> = {}) {
    ecs_.template emplace<system>(entity_, typename system::each_config<FuncT, Policy, void, void>{.handler = std::forward<FuncT>(func)}, ecs_);
    return *this;
  }

  system_builder& interval(double value) {
    ecs_.template get<system>(entity_).interval = value;
    return *this;
  }

  system_builder& stage(uint32_t value) {
    ecs_.template get<system>(entity_).stage = value;
    return *this;
  }

private:
  entt_ext::ecs&    ecs_;
  asio::io_context& main_io_context_;
  asio::io_context& concurrent_io_context_;
  entt_ext::entity  entity_;
};

template <typename... ComponentsT, typename... ExcludeT>
struct system_builder<entt::get_t<ComponentsT...>, entt::exclude_t<ExcludeT...>> {
  using ViewT = decltype(std::declval<entt_ext::ecs>().template view<ComponentsT...>(entt::exclude_t<ExcludeT...>{}));

  explicit system_builder(entt_ext::ecs& ecs, asio::io_context& main_io_ctx, asio::io_context& concurrent_io_ctx)
    : ecs_(ecs)
    , main_io_context_(main_io_ctx)
    , concurrent_io_context_(concurrent_io_ctx)
    , entity_(ecs.create()) {
  }

  template <typename FuncT, run_policy Policy = run_policy::automatic>
  system_builder& each(FuncT&& func, run_policy_t<Policy> = {}) {
    ecs_.template emplace<system>(entity_,
                                  typename system::each_config<FuncT, Policy, entt::get_t<ComponentsT...>, entt::exclude_t<ExcludeT...>>{
                                      .entity            = entity_,
                                      .handler           = func,
                                      .main_io_ctx       = main_io_context_,
                                      .concurrent_io_ctx = concurrent_io_context_},
                                  ecs_);

    return *this;
  }

  template <typename FuncT>
  system_builder& each_once(FuncT&& func) {
    ecs_.template emplace<system>(entity_,
                                  typename system::each_config<FuncT, run_policy::once, entt::get_t<ComponentsT...>, entt::exclude_t<ExcludeT...>>{
                                      .entity            = entity_,
                                      .handler           = func,
                                      .main_io_ctx       = main_io_context_,
                                      .concurrent_io_ctx = concurrent_io_context_},
                                  ecs_);

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
    ecs_.template get<system>(entity_).interval = value;
    return *this;
  }

  system_builder& stage(uint32_t value) {
    ecs_.template get<system>(entity_).stage = value;
    return *this;
  }

private:
  template <typename FuncT, run_policy Policy>
  system_builder& run_impl(FuncT&& func, run_policy_t<Policy> = {}) {

    ecs_.template emplace<system>(entity_,
                                  typename system::run_config<FuncT, Policy, entt::get_t<ComponentsT...>, entt::exclude_t<ExcludeT...>>{
                                      .entity            = entity_,
                                      .handler           = func,
                                      .main_io_ctx       = main_io_context_,
                                      .concurrent_io_ctx = concurrent_io_context_},
                                  ecs_);
    return *this;
  }

private:
  entt_ext::ecs&    ecs_;
  asio::io_context& main_io_context_;
  asio::io_context& concurrent_io_context_;
  entt_ext::entity  entity_;
};
