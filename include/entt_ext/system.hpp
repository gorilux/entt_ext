// Type trait to detect children_view
template <typename T>
struct is_children_view : std::false_type {};

template <typename Type, typename... Others>
struct is_children_view<children_view<Type, Others...>> : std::true_type {};

template <typename T>
inline constexpr bool is_children_view_v = is_children_view<T>::value;

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
            co_await handler(ecs, self, dt, view);
            co_await ecs.remove_deferred<running<system_tag>>(entity);
          },
          asio::detached);
      co_return true;
    };
  }

  template <typename FuncT, run_policy Policy, bool IsOnce, typename... ComponentsT, typename... ExcludeT>
  static auto create_each_parallel_async(each_config<FuncT, Policy, entt::get_t<ComponentsT...>, entt::exclude_t<ExcludeT...>>&& cfg) {
    // Check if we have any children_view types at compile time
    constexpr bool has_children_views = (is_children_view_v<ComponentsT> || ...);

    if constexpr (has_children_views) {
      // Filter out children_view types for the actual view
      using regular_components  = typename filter_children_views<ComponentsT...>::type;
      using children_view_types = typename extract_children_views<ComponentsT...>::type;

      return create_each_parallel_async_impl_with_children<FuncT, Policy, IsOnce>(std::move(cfg),
                                                                                  regular_components{},
                                                                                  children_view_types{},
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
    // Check if we have any children_view types at compile time
    constexpr bool has_children_views = (is_children_view_v<ComponentsT> || ...);

    if constexpr (has_children_views) {
      // Filter out children_view types for the actual view
      using regular_components  = typename filter_children_views<ComponentsT...>::type;
      using children_view_types = typename extract_children_views<ComponentsT...>::type;

      return create_each_sequential_async_impl_with_children<FuncT, Policy, IsOnce>(std::move(cfg),
                                                                                    regular_components{},
                                                                                    children_view_types{},
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
    // Check if we have any children_view types at compile time
    constexpr bool has_children_views = (is_children_view_v<ComponentsT> || ...);

    if constexpr (has_children_views) {
      // Filter out children_view types for the actual view
      using regular_components  = typename filter_children_views<ComponentsT...>::type;
      using children_view_types = typename extract_children_views<ComponentsT...>::type;

      return create_each_sequential_sync_impl_with_children<FuncT, Policy, IsOnce>(std::move(cfg),
                                                                                   regular_components{},
                                                                                   children_view_types{},
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
    // Check if we have any children_view types at compile time
    constexpr bool has_children_views = (is_children_view_v<ComponentsT> || ...);

    if constexpr (has_children_views) {
      // Filter out children_view types for the actual view
      using regular_components  = typename filter_children_views<ComponentsT...>::type;
      using children_view_types = typename extract_children_views<ComponentsT...>::type;

      return create_each_detached_impl_with_children<FuncT, Policy, IsOnce>(std::move(cfg),
                                                                            regular_components{},
                                                                            children_view_types{},
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

  // Helper for creating detached system with children_view types
  template <typename FuncT, run_policy Policy, bool IsOnce, typename... RegularComponents, typename... ChildViewTypes, typename... ExcludeT>
  static auto create_each_detached_impl_with_children(
      each_config<FuncT, Policy, entt::get_t<RegularComponents..., ChildViewTypes...>, entt::exclude_t<ExcludeT...>>&& cfg,
      std::tuple<RegularComponents...>,
      std::tuple<ChildViewTypes...>,
      entt::exclude_t<ExcludeT...>) {

    return [cfg = std::move(cfg)](system& self, ecs_type& ecs, double dt) mutable -> asio::awaitable<bool> {
      auto view = ecs.template view<RegularComponents...>(entt::exclude_t<running<FuncT>, delete_later, running<FuncT, run_once_tag>, ExcludeT...>{});

      if (view.begin() == view.end()) {
        co_return false;
      }

      co_await asio::this_coro::executor;

      for (auto entry : view.each()) {
        auto entity = std::get<0>(entry);

        if (!ecs.valid(entity) || ecs.template any_of<running<FuncT, each_tag>, running<each_tag>>(entity)) {
          continue;
        }

        // Build the children_range objects for each children_view type
        auto children_ranges = std::make_tuple(create_children_range<ChildViewTypes>(ecs, entity)...);

        // Skip if any children_range is empty
        bool should_skip = std::apply(
            [](auto&... ranges) {
              return any_children_range_empty(ranges...);
            },
            children_ranges);
        if (should_skip) {
          continue;
        }

        ecs.template emplace<running<each_tag>>(entity);
        ecs.template emplace<running<FuncT, each_tag>>(entity);
        if constexpr (IsOnce) {
          ecs.template emplace<running<FuncT, run_once_tag>>(entity);
        }

        asio::co_spawn(
            cfg.concurrent_io_ctx,
            [&ecs, entity, &self, dt, handler = cfg.handler, entry, children_ranges = std::move(children_ranges)]() mutable -> asio::awaitable<void> {
              co_await asio::this_coro::executor;

              // Combine entry tuple with children_ranges tuple and call handler
              // We need to unpack children_ranges into references
              auto invoke_with_children = [&](auto&... child_ranges) -> asio::awaitable<void> {
                auto full_args = std::tuple_cat(std::tie(ecs, self, dt), entry, std::tie(child_ranges...));
                co_return co_await std::apply(handler, std::move(full_args));
              };
              co_await std::apply(invoke_with_children, children_ranges);

              co_await ecs.remove_deferred<running<each_tag>>(entity);
              co_await ecs.remove_deferred<running<FuncT, each_tag>>(entity);
              co_return;
            },
            asio::detached);
      }
      co_return true;
    };
  }

  // Helper for creating parallel async system with children_view types
  template <typename FuncT, run_policy Policy, bool IsOnce, typename... RegularComponents, typename... ChildViewTypes, typename... ExcludeT>
  static auto create_each_parallel_async_impl_with_children(
      each_config<FuncT, Policy, entt::get_t<RegularComponents..., ChildViewTypes...>, entt::exclude_t<ExcludeT...>>&& cfg,
      std::tuple<RegularComponents...>,
      std::tuple<ChildViewTypes...>,
      entt::exclude_t<ExcludeT...>) {

    return [cfg = std::move(cfg)](system& self, ecs_type& ecs, double dt) mutable -> asio::awaitable<bool> {
      auto view = ecs.template view<RegularComponents...>(entt::exclude_t<running<FuncT>, delete_later, running<FuncT, run_once_tag>, ExcludeT...>{});

      if (view.begin() == view.end()) {
        co_return false;
      }

      // Collect all operations with children_ranges
      std::vector<std::function<asio::awaitable<void>()>> ops;

      for (auto entry : view.each()) {
        auto entity = std::get<0>(entry);

        // Build the children_range objects for each children_view type
        auto children_ranges = std::make_tuple(create_children_range<ChildViewTypes>(ecs, entity)...);

        // Skip if any children_range is empty
        bool should_skip = std::apply(
            [](auto&... ranges) {
              return any_children_range_empty(ranges...);
            },
            children_ranges);
        if (should_skip) {
          continue;
        }

        ops.push_back(
            [&ecs, &self, dt, handler = cfg.handler, entry, children_ranges = std::move(children_ranges)]() mutable -> asio::awaitable<void> {
              // We need to unpack children_ranges into references
              auto invoke_with_children = [&](auto&... child_ranges) -> asio::awaitable<void> {
                auto full_args = std::tuple_cat(std::tie(ecs, self, dt), entry, std::tie(child_ranges...));
                co_return co_await std::apply(handler, std::move(full_args));
              };
              co_await std::apply(invoke_with_children, children_ranges);
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

  // Helper for creating sequential async system with children_view types
  template <typename FuncT, run_policy Policy, bool IsOnce, typename... RegularComponents, typename... ChildViewTypes, typename... ExcludeT>
  static auto create_each_sequential_async_impl_with_children(
      each_config<FuncT, Policy, entt::get_t<RegularComponents..., ChildViewTypes...>, entt::exclude_t<ExcludeT...>>&& cfg,
      std::tuple<RegularComponents...>,
      std::tuple<ChildViewTypes...>,
      entt::exclude_t<ExcludeT...>) {

    return [cfg = std::move(cfg)](system& self, ecs_type& ecs, double dt) mutable -> asio::awaitable<bool> {
      auto view = ecs.template view<RegularComponents...>(entt::exclude_t<running<FuncT>, delete_later, running<FuncT, run_once_tag>, ExcludeT...>{});

      if (view.begin() == view.end()) {
        co_return false;
      }

      for (auto entry : view.each()) {
        auto entity = std::get<0>(entry);

        // Build the children_range objects for each children_view type
        auto children_ranges = std::make_tuple(create_children_range<ChildViewTypes>(ecs, entity)...);

        // Skip if any children_range is empty
        bool should_skip = std::apply(
            [](auto&... ranges) {
              return any_children_range_empty(ranges...);
            },
            children_ranges);
        if (should_skip) {
          continue;
        }

        // We need to unpack children_ranges into references
        auto invoke_with_children = [&](auto&... child_ranges) -> asio::awaitable<void> {
          auto full_args = std::tuple_cat(std::tie(ecs, self, dt), entry, std::tie(child_ranges...));
          co_return co_await std::apply(cfg.handler, std::move(full_args));
        };
        co_await std::apply(invoke_with_children, children_ranges);

        if constexpr (IsOnce) {
          ecs.template emplace<running<FuncT, run_once_tag>>(entity);
        }
      }
      co_return true;
    };
  }

  // Helper for creating sequential sync system with children_view types
  template <typename FuncT, run_policy Policy, bool IsOnce, typename... RegularComponents, typename... ChildViewTypes, typename... ExcludeT>
  static auto create_each_sequential_sync_impl_with_children(
      each_config<FuncT, Policy, entt::get_t<RegularComponents..., ChildViewTypes...>, entt::exclude_t<ExcludeT...>>&& cfg,
      std::tuple<RegularComponents...>,
      std::tuple<ChildViewTypes...>,
      entt::exclude_t<ExcludeT...>) {

    return [cfg = std::move(cfg)](system& self, ecs_type& ecs, double dt) mutable -> asio::awaitable<bool> {
      auto view = ecs.template view<RegularComponents...>(entt::exclude_t<running<FuncT>, delete_later, running<FuncT, run_once_tag>, ExcludeT...>{});

      if (view.begin() == view.end()) {
        co_return false;
      }

      for (auto entry : view.each()) {
        auto entity = std::get<0>(entry);

        // Build the children_range objects for each children_view type
        auto children_ranges = std::make_tuple(create_children_range<ChildViewTypes>(ecs, entity)...);

        // Skip if any children_range is empty
        bool should_skip = std::apply(
            [](auto&... ranges) {
              return any_children_range_empty(ranges...);
            },
            children_ranges);
        if (should_skip) {
          continue;
        }

        // We need to unpack children_ranges into references
        auto invoke_with_children = [&](auto&... child_ranges) {
          auto full_args = std::tuple_cat(std::tie(ecs, self, dt), entry, std::tie(child_ranges...));
          std::apply(
              [&](auto&&... args) {
                cfg.handler(std::forward<decltype(args)>(args)...);
              },
              full_args);
        };
        std::apply(invoke_with_children, children_ranges);

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

      if (!ecs.valid(entity) || ecs.template any_of<running<FuncT, each_tag>, running<each_tag>>(entity)) {
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

            co_await ecs.remove_deferred<running<each_tag>>(entity);
            co_await ecs.remove_deferred<running<FuncT, each_tag>>(entity);
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
