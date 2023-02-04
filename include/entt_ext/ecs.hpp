#pragma once

#include "core.hpp"
#include "ecs_fwd.hpp"
#ifndef __EMSCRIPTEN__
#include "nodes.hpp"
#endif

#include "index_map.hpp"
#include "index_set.hpp"
#include "return_type.hpp"
#include "type_name.hpp"

#include <cereal/archives/portable_binary.hpp>
#include <cereal/cereal.hpp>
#include <entt/entity/registry.hpp>
#include <entt/entity/snapshot.hpp>
#include <entt/entity/storage.hpp>
#include <entt/graph/adjacency_matrix.hpp>

#include <chrono>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <thread>
#include <tuple>
#include <type_traits>

namespace entt_ext {

struct state_persistance_header {
  std::uint32_t version = 0;

  template <typename ArchiveT>
  void serialize(ArchiveT& ar, std::uint32_t const class_version) {
    ar(version);
  }
};

// template <typename... Type>
// struct sync_t final : entt::type_list<Type...> {
//   /*! @brief Default constructor. */
//   explicit constexpr sync_t() {
//   }
// };

// template <typename... Type>
// inline constexpr sync_t<Type...> sync{};
using handler_id = size_t;

template <typename Type, typename EcsT>
class component {

  using handler_type = std::function<void(EcsT&, entt_ext::entity, Type&)>;

public:
  component(EcsT& ecs, entt_ext::entity entt) {
  }

  ~component() = default;

  std::string_view name() const {
    return type_name<Type>();
  }

  template <typename FuncT>
  handler_id on_construct(FuncT&& func) {
    auto id = next_handler_id++;
    on_construct_handlers.emplace(id, func);
    return id;
  }

  template <typename FuncT>
  handler_id on_destroy(FuncT&& func) {
    auto id = next_handler_id++;
    on_destroy_handlers.emplace(id, func);
    return id;
  }

  template <typename FuncT>
  handler_id on_update(FuncT&& func) {
    auto id = next_handler_id++;
    on_update_handlers.emplace(id, func);
    return id;
  }

  bool on_construct_disconnect(handler_id id) {
    if (auto it = on_construct_handlers.find(id); it != on_construct_handlers.end()) {
      on_construct_handlers.erase(it->first);
      return true;
    }
    return false;
  }

  bool on_destroy_disconnect(handler_id id) {
    if (auto it = on_destroy_handlers.find(id); it != on_destroy_handlers.end()) {
      on_destroy_handlers.erase(it->first);
      return true;
    }
    return false;
  }

  bool on_update_disconnect(handler_id id) {
    if (auto it = on_update_handlers.find(id); it != on_update_handlers.end()) {
      on_update_handlers.erase(it->first);
      return true;
    }
    return false;
  }

  void dispatch_on_construct(EcsT& ecs, entt_ext::entity entt) {

    // spdlog::debug("on_construct: {}:{}", static_cast<size_t>(entt), name());
    for (auto& [id, handler] : on_construct_handlers) {
      if constexpr (entt::component_traits<Type>::page_size > 0u) {
        handler(ecs, entt, ecs.template get<Type>(entt));
      } else {
        Type t;
        handler(ecs, entt, t);
      }
    }
  }

  void dispatch_on_destroy(EcsT& ecs, entt_ext::entity entt) {

    // spdlog::debug("on_destroy: {}:{}", static_cast<size_t>(entt), name());
    for (auto& [id, handler] : on_destroy_handlers) {
      if constexpr (entt::component_traits<Type>::page_size > 0u) {
        handler(ecs, entt, ecs.template get<Type>(entt));
      } else {
        Type t;
        handler(ecs, entt, t);
      }
    }
  }

  void dispatch_on_update(EcsT& ecs, entt_ext::entity entt) {

    // spdlog::debug("on_update: {}:{}", static_cast<size_t>(entt), name());
    for (auto& [id, handler] : on_update_handlers) {
      if constexpr (entt::component_traits<Type>::page_size > 0u) {
        handler(ecs, entt, ecs.template get<Type>(entt));
      } else {
        Type t;
        handler(ecs, entt, t);
      }
    }
  }

private:
  handler_id                          next_handler_id = 0;
  index_map<handler_id, handler_type> on_construct_handlers;
  index_map<handler_id, handler_type> on_destroy_handlers;
  index_map<handler_id, handler_type> on_update_handlers;
};

template <typename Type>
struct child final {
  entt_ext::entity parent;
};

template <typename Type>
struct parent final {
  entt_ext::entity child;
};

template <typename Type>
using children = index_set<entt_ext::entity, Type>;

template <typename EcsT, typename Type, typename... Types>
class children_range {
  EcsT&           ecs;
  children<Type>& ir;

public:
  children_range(EcsT& ecs, children<Type>& ir)
    : ecs(ecs)
    , ir(ir) {
  }

  struct iterator {
    EcsT&                             ecs;
    typename children<Type>::iterator it;
    typename children<Type>::iterator end;

    iterator(EcsT& ecs, typename children<Type>::iterator it, typename children<Type>::iterator end)
      : ecs(ecs)
      , it(it)
      , end(end) {
      // Skip entities that don't have all the required components
      skip_invalid();
    }

    void skip_invalid() {
      if constexpr (entt::component_traits<Type>::page_size > 0u) {
        while (it != end && !ecs.template all_of<Type, Types...>(*it)) {
          ++it;
        }
      } else {
        while (it != end && !ecs.template all_of<Types...>(*it)) {
          ++it;
        }
      }
    }

    iterator& operator++() {
      ++it;
      skip_invalid(); // Skip entities that don't have all the required components
      return *this;
    }

    bool operator!=(const iterator& other) const {
      return it != other.it;
    }

    auto operator*() {
      if constexpr (sizeof...(Types) > 0) {
        if constexpr (entt::component_traits<Type>::page_size > 0u) {
          return std::tuple_cat(std::tie(*it), ecs.template get<Type, Types...>(*it));
        } else {
          if constexpr (sizeof...(Types) == 1u) {
            return std::make_tuple(*it, ecs.template get<Types...>(*it));
          } else {
            return std::tuple_cat(std::tie(*it), ecs.template get<Types...>(*it));
          }
        }
      } else {
        if constexpr (entt::component_traits<Type>::page_size > 0u) {
          return std::make_tuple(*it, std::ref(ecs.template get<Type>(*it)));
        } else {
          return std::make_tuple(*it);
        }
      }
    }
  };

  struct iterable_proxy {
    EcsT&           ecs;
    children<Type>& ir;

    iterator begin() {
      return iterator{ecs, ir.begin(), ir.end()};
    }

    iterator end() {
      return iterator{ecs, ir.end(), ir.end()};
    }
  };

  iterable_proxy each() {
    return iterable_proxy{ecs, ir};
  }
};

template <typename Type, typename EcsT>
struct module_wrapper : public Type {
  module_wrapper(EcsT& ecs)
    : Type(ecs)
    , entity(ecs.create()) {
  }
  entt_ext::entity entity;
};

enum class run_policy {
  automatic,
  sequential,
  detached,
  parallel,
  once
};

template <typename... TagT>
struct running {};

struct each_tag {};
struct system_tag {};
struct run_once_tag {};

template <auto Policy>
struct run_policy_t {
  static constexpr run_policy value = Policy;
  constexpr run_policy_t() {
  }
};

template <auto Policy>
inline constexpr run_policy_t<Policy> make_run_policy{};

using clock      = std::chrono::high_resolution_clock;
using time_point = std::chrono::time_point<clock>;

inline auto now() -> time_point {
  return clock::now();
}

struct stage {
  enum val : uint32_t {
    input       = 1000,
    update      = 2000,
    post_update = 3000,
    draw        = 4000,
    pre_render  = 5000,
    render      = 6000,
    post_render = 7000,
    cleanup     = 0xffffffff
  };
};

template <typename, typename>
struct system_builder;

class ecs {
public:
  using registry_type  = entt::basic_registry<entt_ext::entity, std::allocator<entt_ext::entity>>;
  using allocator_type = typename registry_type::allocator_type;
  using entity_type    = typename registry_type::entity_type;
  using size_type      = typename registry_type::size_type;
  using version_type   = typename registry_type::version_type;
  // using traits_type           = typename registry_type::traits_type;
  using self_type             = ecs;
  using alloc_traits          = std::allocator_traits<std::allocator<entt_ext::entity>>;
  using adjacency_matrix_type = entt::adjacency_matrix<entt::directed_tag>;

  ecs(std::string const& persistance_filename = "");
  explicit ecs(const allocator_type& allocator, std::string const& persistance_filename = "");
  ecs(ecs&& other) noexcept;
  ecs& operator=(ecs&& other) noexcept;

  [[nodiscard]] entity_type create();
  [[nodiscard]] entity_type create(const entity_type hint);

  template <typename It>
  void create(It first, It last) {
    return m_registry.create(first, last);
  }

  version_type destroy(const entity_type entt);
  version_type destroy(const entity_type entt, const version_type version);

  template <typename Type, typename... ArgsT>
  decltype(auto) component(ArgsT&&... args);

  template <typename Type, typename... Args>
  decltype(auto) emplace(const entity_type entt, Args&&... args);

  template <typename Type, typename... Args>
  auto emplace_if_not_exists(const entity_type entt, Args&&... args) -> decltype(entt::component_traits<Type>::page_size > 0u, std::pair<Type&, bool>{}) {
    if (auto* component = try_get<Type>(entt); component) {
      return std::make_pair(std::ref(*component), false);
    }
    return std::make_pair(emplace<Type>(entt, std::forward<Args>(args)...), true);
  }

  template <typename Type, typename... Args>
  auto emplace_if_not_exists(const entity_type entt, Args&&... args) -> decltype(entt::component_traits<Type>::page_size == 0u, bool{}) {
    if (m_registry.template storage<Type>().contains(entt)) {
      return false;
    }
    emplace<Type>(entt, std::forward<Args>(args)...);
    return true;
  }

  template <typename Type, typename It>
  void insert(It first, It last, const Type& value = {});

  template <typename Type, typename EIt, typename CIt, typename = std::enable_if_t<std::is_same_v<typename std::iterator_traits<CIt>::value_type, Type>>>
  void insert(EIt first, EIt last, CIt from);

  template <typename Type, typename... Args>
  decltype(auto) emplace_or_replace(const entity_type entt, Args&&... args);

  template <typename Type, typename... Func>
  decltype(auto) patch(const entity_type entt, Func&&... func) {
    return m_registry.template patch<Type>(entt, std::forward<Func>(func)...);
  }

  template <typename Type, typename... Args>
  decltype(auto) replace(const entity_type entt, Args&&... args) {
    return m_registry.template replace<Type>(entt, std::forward<Args>(args)...);
  }

  template <typename Type, typename... Other>
  size_type remove(const entity_type entt) {
    return m_registry.template remove<Type, Other...>(entt);
  }

  template <typename Type, typename... Other, typename It>
  size_type remove(It first, It last) {
    return m_registry.template remove<Type, Other...>(first, last);
  }

  template <typename Type, typename... Other>
  void erase(const entity_type entt) {
    m_registry.template erase<Type, Other...>(entt);
  }

  template <typename Type, typename... Other, typename It>
  void erase(It first, It last) {
    m_registry.template erase<Type, Other...>(first, last);
  }

  template <typename Func>
  void erase_if(const entity_type entt, Func func) {
    m_registry.erase_if(entt, func);
  }

  template <typename... Type>
  void compact() {
    return m_registry.template compact<Type...>();
  }

  template <typename... Type>
  [[nodiscard]] bool all_of(const entity_type entt) const {
    return m_registry.template all_of<Type...>(entt);
  }

  template <typename... Type>
  [[nodiscard]] bool any_of(const entity_type entt) const {
    return m_registry.template any_of<Type...>(entt);
  }

  template <typename... Type>
  [[nodiscard]] decltype(auto) get([[maybe_unused]] const entity_type entt) const {
    return m_registry.template get<Type...>(entt);
  }

  /*! @copydoc get */
  template <typename... Type>
  [[nodiscard]] decltype(auto) get([[maybe_unused]] const entity_type entt) {
    return m_registry.template get<Type...>(entt);
  }

  template <typename Type, typename... Args>
  [[nodiscard]] decltype(auto) get_or_emplace(const entity_type entt, Args&&... args);

  template <typename... Type>
  [[nodiscard]] auto try_get([[maybe_unused]] const entity_type entt) const {
    return m_registry.template try_get<Type...>(entt);
  }

  template <typename... Type>
  [[nodiscard]] auto try_get([[maybe_unused]] const entity_type entt) {
    return m_registry.template try_get<Type...>(entt);
  }

  template <typename... Type>
  void clear() {
    return m_registry.template clear<Type...>();
  }

  [[nodiscard]] bool orphan(const entity_type entt) const {
    return m_registry.orphan(entt);
  }

  template <typename Type, typename... Other, typename... Exclude>
  [[nodiscard]] auto view(entt::exclude_t<Exclude...> = entt::exclude_t{}) {
    return m_registry.template view<Type, Other...>(entt::exclude_t<Exclude...>{});
  }

  template <typename Type, typename... Other, typename... Exclude>
  [[nodiscard]] auto view(entt::exclude_t<Exclude...> = entt::exclude_t{}) const {
    return m_registry.template view<Type, Other...>(entt::exclude_t<Exclude...>{});
  }

  template <typename... Owned, typename... Get, typename... Exclude>
  auto group(entt::get_t<Get...> = entt::get_t{}, entt::exclude_t<Exclude...> = entt::exclude_t{}) {
    return m_registry.template group<Owned...>(entt::get_t<Get...>{}, entt::exclude_t<Exclude...>{});
  }

  template <typename Type, typename... ArgsT>
  decltype(auto) get_or_emplace(ArgsT&&... args) {
    return get_or_emplace<Type>(m_global_entity, std::forward<ArgsT>(args)...);
  }

  template <typename Type>
  bool contains() const {
    return m_registry.template any_of<Type>(m_global_entity);
  }

  template <typename Type>
  decltype(auto) get() const {
    return m_registry.template get<Type>(m_global_entity);
  }

  template <typename Type>
  decltype(auto) get() {
    return m_registry.template get<Type>(m_global_entity);
  }

  void delete_later(const entity_type entt);

  size_t index(const entity_type entt) const {
    return m_registry.template storage<entity_type>()->index(entt);
  }

  entity_type at(size_t pos) const {
    return m_registry.template storage<entity_type>()->data()[pos];
  }

  entt::registry& registry() {
    return m_registry;
  }

  const entt::registry& registry() const {
    return m_registry;
  }

  entt_ext::entity get_global_entity() const {
    return m_global_entity;
  }

  template <typename Type, typename Compare, typename Sort = entt::std_sort, typename... Args>
  void sort(Compare compare, Sort algo = Sort{}, Args&&... args) {
    m_registry.template sort<Type>(compare, algo, std::forward<Args>(args)...);
  }

  template <typename To, typename From>
  void sort() {
    m_registry.template sort<To, From>();
  }

  template <typename Type, typename It>
  void sort_as(It first, It last) {
    auto& storage = m_registry.template storage<Type>();
    storage.sort_as(first, last);
  }

  [[nodiscard]] bool valid(const entity_type entt) const {
    return m_registry.valid(entt);
  }

  template <typename Type>
  void dispatch_on_construct(registry_type& registry, entt_ext::entity entt) {
    for (auto [_, component] : registry.template view<entt_ext::component<Type, self_type>>().each()) {
      component.dispatch_on_construct(*this, entt);
    }
  }

  template <typename Type>
  void dispatch_on_destroy(registry_type& registry, entt_ext::entity entt) {
    for (auto [_, component] : registry.template view<entt_ext::component<Type, self_type>>().each()) {
      component.dispatch_on_destroy(*this, entt);
    }
  }

  template <typename Type>
  void dispatch_on_update(registry_type& registry, entt_ext::entity entt) {
    for (auto [_, component] : registry.template view<entt_ext::component<Type, self_type>>().each()) {
      component.dispatch_on_update(*this, entt);
    }
  }

  template <typename Type, typename... ArgsT>
  decltype(auto) emplace_child(entt_ext::entity parent, entt_ext::entity chld, ArgsT&&... args) {
    get_or_emplace<children<Type>>(parent).insert(chld);

    emplace<child<Type>>(chld, parent);
    return emplace<Type>(chld, std::forward<ArgsT>(args)...);
  }

  template <typename Type, typename... ArgsT>
  decltype(auto) emplace_or_replace_child(entt_ext::entity parent, entt_ext::entity chld, ArgsT&&... args) {
    get_or_emplace<children<Type>>(parent).insert(chld);

    emplace_or_replace<child<Type>>(chld, parent);
    return emplace_or_replace<Type>(chld, std::forward<ArgsT>(args)...);
  }

  template <typename Type, typename... ArgsT>
  decltype(auto) emplace_child(entt_ext::entity parent, ArgsT&&... args) {
    return emplace_child<Type>(parent, create(), std::forward<ArgsT>(args)...);
  }

  template <typename Type>
  void assign_child(entt_ext::entity parent, entt_ext::entity chld) {
    get_or_emplace<children<Type>>(parent).insert(chld);

    if (auto child_ptr = try_get<child<Type>>(chld); child_ptr) {
      if (child_ptr->parent == parent) {
        return;
      }
      if (auto prev_relationship = try_get<children<Type>>(child_ptr->parent); prev_relationship && prev_relationship->contains(chld)) {
        prev_relationship->erase(chld);
      }
      child_ptr->parent = parent;
    } else {
      emplace<child<Type>>(chld, parent);
    }
  }

  template <typename Type, typename... Others, typename FuncT>
  void each_child(entt_ext::entity parent, children<Type>& chldrn, FuncT&& func) {
    auto v = view<Type, Others...>();

    for (auto entity : chldrn) {
      if (v.contains(entity)) {
        std::apply(func, std::tuple_cat(std::tie(*this, parent, entity), v.get(entity)));
      }
    }
  }

  template <typename Type, typename... Others, typename FuncT>
  void each_child(entt_ext::entity parent, FuncT&& func) {
    if (auto children_ptr = try_get<children<Type>>(parent); children_ptr) {
      each_child<Type, Others...>(parent, *children_ptr, std::move(func));
    }
  }

  template <typename Type, typename... Others>
  children_range<self_type, Type, Others...> children_view(entt_ext::entity parent) {
    return children_range<self_type, Type, Others...>(*this, get_or_emplace<children<Type>>(parent));
  }

  // template <typename... Type, typename ViewT, typename FuncT>
  // inline void each_child(ViewT& view, entt_ext::entity parent, FuncT&& func) {
  //   each_child(view.template get<children<Type...>>(parent), std::move(func));
  // }

  template <typename FuncT>
  auto post(FuncT&& func) -> void {
    if constexpr (std::is_invocable_v<FuncT, self_type&>) {
      asio::post(main_io_context(), [f = std::move(func), this]() {
        f(*this);
      });
    } else {
      asio::post(main_io_context(), std::forward<FuncT>(func));
    }
  }

  template <typename FuncT>
  auto post_and_await(FuncT&& func) -> asio::awaitable<void> {
    co_return co_await asio::async_compose<decltype(asio::use_awaitable), void()>(
        [this, func = std::move(func)](auto&& self) {
          asio::defer(main_io_context(), [self = std::move(self), this, func = std::move(func)]() mutable {
            func(*this);
            self.complete();
          });
        },
        asio::use_awaitable);
    ;
  }

  template <typename FuncT>
  void defer(FuncT&& func) {
    asio::post(main_io_context(), [func = std::move(func), this]() mutable {
      if constexpr (std::is_same_v<return_type_t<FuncT>, asio::awaitable<void>>) {
        m_defered_tasks.push_back(std::forward<FuncT>(func));
      } else {
        m_defered_tasks.push_back([func = std::move(func)](entt_ext::ecs& ecs) -> asio::awaitable<void> {
          func(ecs);
          co_return;
        });
      }
    });
  }

  template <typename Type, typename... Other>
  void remove_deferred(const entity_type entt) {
    defer([this, entt](entt_ext::ecs& ecs) {
      ecs.template remove<Type, Other...>(entt);
    });
  }

  template <typename Type, typename... Other, typename It>
  void remove_deferred(It first, It last) {
    defer([this, first, last](entt_ext::ecs& ecs) {
      ecs.template remove<Type, Other...>(first, last);
    });
  }

  void run(int timeout_ms = 10, size_t concurrency = std::thread::hardware_concurrency());
  void stop();
  auto main_io_context() -> asio::io_context&;
  auto concurrent_io_context() -> asio::io_context&;

  template <typename... ComponentsT, typename... ExcludeT>
  auto system(entt::exclude_t<ExcludeT...>) -> system_builder<entt::get_t<ComponentsT...>, entt::exclude_t<ExcludeT...>>;

  template <typename... ComponentsT>
  auto system() -> system_builder<entt::get_t<ComponentsT...>, entt::exclude_t<>>;

  template <typename ArchiveT, typename... ComponentsT>
  asio::awaitable<void> load_snapshot(ArchiveT& ar) {

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
  asio::awaitable<void> save_snapshot(ArchiveT& ar) const {
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

#ifndef __EMSCRIPTEN__
  auto graph() -> entt_ext::graph<self_type>&;
  auto graph() const -> const entt_ext::graph<self_type>&;

  template <typename ArchiveT, typename... ComponentsT>
  asio::awaitable<void> load_graph_snapshot(ArchiveT& ar) {

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
  asio::awaitable<void> save_graph_snapshot(ArchiveT& ar) const {
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
  asio::awaitable<bool> merge_graph_snapshot(ArchiveT& ar) {
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

  template <typename ArchiveT, typename... ComponentsT>
  asio::awaitable<bool> merge_snapshot(ArchiveT& ar) {
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

  auto create_async() -> asio::awaitable<entity_type>;

  template <typename Type, typename... Args>
  auto emplace_async(entity_type entt, Args&&... args) -> std::enable_if_t<!std::is_same_v<void, std::decay_t<decltype(std::declval<ecs>().template emplace<Type>(entt, std::forward<Args>(args)...))>>, asio::awaitable<std::reference_wrapper<Type>>> {
    co_return co_await asio::async_compose<decltype(asio::use_awaitable), void(std::reference_wrapper<Type>)>(
        [entt, this, ... args = std::forward<Args>(args)](auto&& self) mutable {
          asio::post(main_io_context(), [self = std::move(self), entt, this, ... args = std::forward<Args>(args)]() mutable {
            self.complete(std::ref(emplace<Type>(entt, std::forward<Args>(args)...)));
          });
        },
        asio::use_awaitable);
  }

  template <typename Type, typename... Args>
  auto emplace_async(entity_type entt, Args&&... args) -> std::enable_if_t<std::is_same_v<void, std::decay_t<decltype(std::declval<ecs>().template emplace<Type>(entt, std::forward<Args>(args)...))>>, asio::awaitable<void>> {
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
  auto emplace_if_not_exists_async(entity_type entt, Args&&... args) -> std::enable_if_t<!std::is_same_v<void, std::decay_t<decltype(std::declval<ecs>().template emplace_if_not_exists<Type>(entt, std::forward<Args>(args)...))>>, asio::awaitable<void>> {
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
  auto get_async(entity_type entt) const -> std::enable_if_t<!std::is_same_v<void, std::decay_t<decltype(std::declval<ecs>().template get<Type>(entt))>>, asio::awaitable<std::reference_wrapper<Type>>> {
    co_return co_await asio::async_compose<decltype(asio::use_awaitable), void(std::reference_wrapper<Type>)>(
        [entt, this](auto&& self) {
          asio::post(const_cast<ecs*>(this)->main_io_context(), [self = std::move(self), entt, this]() mutable {
            self.complete(std::ref(get<Type>(entt)));
          });
        },
        asio::use_awaitable);
  }

  template <typename Type, typename... Args>
  asio::awaitable<size_t> remove_async(entity_type entt) {
    co_return co_await asio::async_compose<decltype(asio::use_awaitable), void(size_t)>(
        [entt, this](auto&& self) {
          asio::post(main_io_context(), [self = std::move(self), entt, this]() mutable {
            self.complete(remove<Type, Args...>(entt));
          });
        },
        asio::use_awaitable);
  }

  template <typename Type, typename... Other, typename It>
  asio::awaitable<size_t> remove_async(It first, It last) {
    co_return co_await asio::async_compose<decltype(asio::use_awaitable), void(size_t)>(
        [first, last, this](auto&& self) {
          asio::post(main_io_context(), [self = std::move(self), first, last, this]() mutable {
            self.complete(remove<Type, Other...>(first, last));
          });
        },
        asio::use_awaitable);
  }

  template <typename Type, typename... Args>
  auto emplace_or_replace_async(entity_type entt, Args... args) -> std::enable_if_t<!std::is_same_v<void, std::decay_t<decltype(std::declval<ecs>().template emplace_or_replace<Type>(entt, std::forward<Args>(args)...))>>, asio::awaitable<std::reference_wrapper<Type>>> {
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
  auto emplace_or_replace_async(entity_type entt, Args&&... args) -> std::enable_if_t<std::is_same_v<void, std::decay_t<decltype(std::declval<ecs>().template emplace_or_replace<Type>(entt, std::forward<Args>(args)...))>>, asio::awaitable<void>> {
    co_return co_await asio::async_compose<decltype(asio::use_awaitable), void()>(
        [entt, this, args...](auto&& self) {
          asio::post(main_io_context(), [self = std::move(self), entt, this, args...]() mutable {
            emplace_or_replace<Type>(entt, args...);
            self.complete();
          });
        },
        asio::use_awaitable);
  }

private:
  struct main_context_tag {};
  struct concurrent_context_tag {};
  struct settings_filename {
    std::string filename;
  };

  void load_state(std::string const& filename);

  void save_state();

  static constexpr auto use_nothrow_awaitable = asio::as_tuple(asio::use_awaitable);

  auto run_update_loop(int timeout_ms, size_t concurrency) -> asio::awaitable<void>;

private:
  registry_type           m_registry;
  entt_ext::entity        m_global_entity     = entt_ext::null;
  entt_ext::entity        m_concurrent_entity = entt_ext::null;
  entt::continuous_loader m_continuous_loader;

#ifndef __EMSCRIPTEN__
  entt_ext::graph<self_type> m_graph;
#endif
  std::vector<std::function<asio::awaitable<void>(entt_ext::ecs&)>> m_defered_tasks;
  bool                                                              m_running = true;
};

template <typename T, typename... ArgsT>
auto get_or_emplace(entt_ext::ecs& ecs, ArgsT&&... args) -> T& {
  return ecs.get_or_emplace<T>(ecs.get_global_entity(), std::forward<ArgsT>(args)...);
}

template <typename T>
auto get(entt_ext::ecs& ecs) -> T& {
  return ecs.get<T>(ecs.get_global_entity());
}

template <typename T>
decltype(auto) import(entt_ext::ecs& ecs) {
  return ecs.get_or_emplace<module_wrapper<T, decltype(ecs)>>(ecs);
}

template <typename T>
decltype(auto) module(entt_ext::ecs& ecs) {
  return ecs.get<module_wrapper<T, decltype(ecs)>>();
}

// Include system.hpp to get system_builder definition
#include "system.hpp"

// Now implement the ecs::system methods inline after system_builder is defined
template <typename... ComponentsT, typename... ExcludeT>
inline auto ecs::system(entt::exclude_t<ExcludeT...>) -> system_builder<entt::get_t<ComponentsT...>, entt::exclude_t<ExcludeT...>> {
  return system_builder<entt::get_t<ComponentsT...>, entt::exclude_t<ExcludeT...>>(*this, main_io_context(), concurrent_io_context());
}

template <typename... ComponentsT>
inline auto ecs::system() -> system_builder<entt::get_t<ComponentsT...>, entt::exclude_t<>> {
  return system<ComponentsT...>(entt::exclude_t<>{});
}

// Now implement the ecs::component method after the class definition
template <typename Type, typename... ArgsT>
inline decltype(auto) ecs::component(ArgsT&&... args) {
  using component_type = entt_ext::component<Type, self_type>;

  auto view = m_registry.template view<component_type>();
  if (view.begin() == view.end()) {
    // spdlog::debug("Registering component {}", type_name<Type>());
    auto           entity    = create();
    decltype(auto) component = m_registry.template emplace<component_type>(entity, *this, entity, std::forward<ArgsT>(args)...);
    m_registry.template on_construct<Type>().template connect<&ecs::dispatch_on_construct<Type>>(*this);
    m_registry.template on_destroy<Type>().template connect<&ecs::dispatch_on_destroy<Type>>(*this);
    m_registry.template on_update<Type>().template connect<&ecs::dispatch_on_update<Type>>(*this);

    return component;
  }
  return view.template get<component_type>(view.front());
}

template <typename Type, typename... Args>
inline decltype(auto) ecs::emplace(const entity_type entt, Args&&... args) {
  component<Type>();
  return m_registry.template emplace<Type>(entt, std::forward<Args>(args)...);
}

template <typename Type, typename It>
inline void ecs::insert(It first, It last, const Type& value) {
  component<Type>();
  return m_registry.template insert<Type>(std::move(first), std::move(last), value);
}

template <typename Type, typename EIt, typename CIt, typename>
inline void ecs::insert(EIt first, EIt last, CIt from) {
  component<Type>();
  m_registry.template insert<Type>(first, last, from);
}

template <typename Type, typename... Args>
inline decltype(auto) ecs::emplace_or_replace(const entity_type entt, Args&&... args) {
  component<Type>();
  return m_registry.template emplace_or_replace<Type>(entt, std::forward<Args>(args)...);
}

template <typename Type, typename... Args>
inline decltype(auto) ecs::get_or_emplace(const entity_type entt, Args&&... args) {
  component<Type>();
  return m_registry.template get_or_emplace<Type>(entt, std::forward<Args>(args)...);
}
inline void ecs::delete_later(const entity_type entt) {
  return emplace<entt_ext::delete_later>(entt);
}
} // namespace entt_ext