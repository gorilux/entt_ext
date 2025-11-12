#pragma once

#include "core.hpp"
#include "ecs_fwd.hpp"

#include "component_observer.hpp"
#include "continuous_loader_with_mapping.hpp"
#include "deferred_operations.hpp"
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

#include <boost/asio/experimental/concurrent_channel.hpp>

#include <chrono>
#include <coroutine>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <thread>
#include <tuple>
#include <type_traits>

// #include <spdlog/spdlog.h>

namespace entt_ext {

// Command channel for async deferred operations
// Define as actual class not type alias to allow forward declaration
class command_channel
  : public asio::experimental::concurrent_channel<asio::io_context::executor_type, void(boost::system::error_code, deferred_command)> {
public:
  using base_type = asio::experimental::concurrent_channel<asio::io_context::executor_type, void(boost::system::error_code, deferred_command)>;

  using base_type::base_type; // Inherit constructors
};

struct state_persistance_header {
  std::uint32_t version = 0;

  template <typename ArchiveT>
  void serialize(ArchiveT& ar, std::uint32_t const class_version) {
    ar(version);
  }
};

template <typename Type>
struct parent final {
  entt_ext::entity entity;

  template <typename Archive>
  void serialize(Archive& archive, std::uint32_t const class_version) {
    archive(entity);
  }
};

template <typename Type>
using children = index_set<entt_ext::entity, Type>;

// Marker type for system component declarations to inject children views
template <typename Type, typename... Others>
struct children_view {};

// Entity mapping helper functions for children (index_set<entity, Type>)
// These are free functions that work with the children type alias

// Map entity references from remote to local IDs (for receiving from server)
template <typename Type, typename LoaderT>
void map_entities(children<Type>& child_set, LoaderT const& loader) {
  // Create a new set with mapped entities
  children<Type> mapped_set;

  for (auto const& element : child_set) {
    auto mapped = loader.map(element);
    if (mapped != entt_ext::null) {
      mapped_set.insert(mapped);
    }
  }

  // Swap with the original
  child_set.swap(mapped_set);
}

// Map entity references from local to remote IDs (for sending to server)
template <typename Type, typename LoaderT>
void map_entities_to_remote(children<Type>& child_set, LoaderT const& loader) {
  // Create a new set with mapped entities
  children<Type> mapped_set;

  for (auto const& element : child_set) {
    auto mapped = loader.to_remote(element);
    if (mapped != entt_ext::null) {
      mapped_set.insert(mapped);
    }
  }

  // Swap with the original
  child_set.swap(mapped_set);
}

// Map entity references from remote to local IDs
template <typename Type, typename LoaderT>
void map_entities(parent<Type>& parent, LoaderT const& loader) {
  parent.entity = loader.map(parent.entity);
}

// Map entity references from local to remote IDs (for sending to server)
template <typename Type, typename LoaderT>
void map_entities_to_remote(parent<Type>& parent, LoaderT const& loader) {
  parent.entity = loader.to_remote(parent.entity);
}

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
      // Types... are used only for filtering, we only return Type
      if constexpr (entt::component_traits<Type>::page_size > 0u) {
        return std::make_tuple(*it, std::ref(ecs.template get<Type>(*it)));
      } else {
        return std::make_tuple(*it);
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
    return registry_.create(first, last);
  }

  version_type destroy(const entity_type entt);
  version_type destroy(const entity_type entt, const version_type version);

  template <typename Type>
  decltype(auto) component_observer();

  template <typename Type, typename... Args>
  decltype(auto) emplace(const entity_type entt, Args&&... args);

  template <typename Type, typename... Args>
  auto emplace_if_not_exists(const entity_type entt, Args&&... args)
      -> decltype(entt::component_traits<Type>::page_size > 0u, std::pair<Type&, bool>{}) {
    if (auto* component = try_get<Type>(entt); component) {
      return std::make_pair(std::ref(*component), false);
    }
    return std::make_pair(emplace<Type>(entt, std::forward<Args>(args)...), true);
  }

  template <typename Type, typename... Args>
  auto emplace_if_not_exists(const entity_type entt, Args&&... args) -> decltype(entt::component_traits<Type>::page_size == 0u, bool{}) {
    if (registry_.template storage<Type>().contains(entt)) {
      return false;
    }
    emplace<Type>(entt, std::forward<Args>(args)...);
    return true;
  }

  template <typename Type, typename It>
  void insert(It first, It last, const Type& value = {});

  template <typename Type,
            typename EIt,
            typename CIt,
            typename = std::enable_if_t<std::is_same_v<typename std::iterator_traits<CIt>::value_type, Type>>>
  void insert(EIt first, EIt last, CIt from);

  template <typename Type, typename... Args>
  decltype(auto) emplace_or_replace(const entity_type entt, Args&&... args);

  template <typename Type, typename... Func>
  decltype(auto) patch(const entity_type entt, Func&&... func) {
    return registry_.template patch<Type>(entt, std::forward<Func>(func)...);
  }

  template <typename Type, typename... Args>
  decltype(auto) replace(const entity_type entt, Args&&... args) {
    return registry_.template replace<Type>(entt, std::forward<Args>(args)...);
  }

  template <typename Type, typename... Other>
  size_type remove(const entity_type entt) {
    return registry_.template remove<Type, Other...>(entt);
  }

  template <typename Type, typename... Other, typename It>
  size_type remove(It first, It last) {
    return registry_.template remove<Type, Other...>(first, last);
  }

  template <typename Type, typename... Other>
  void erase(const entity_type entt) {
    registry_.template erase<Type, Other...>(entt);
  }

  template <typename Type, typename... Other, typename It>
  void erase(It first, It last) {
    registry_.template erase<Type, Other...>(first, last);
  }

  template <typename Func>
  void erase_if(const entity_type entt, Func func) {
    registry_.erase_if(entt, func);
  }

  template <typename... Type>
  void compact() {
    return registry_.template compact<Type...>();
  }

  template <typename... Type>
  [[nodiscard]] bool all_of(const entity_type entt) const {
    return registry_.template all_of<Type...>(entt);
  }

  template <typename... Type>
  [[nodiscard]] bool any_of(const entity_type entt) const {
    return registry_.template any_of<Type...>(entt);
  }

  template <typename... Type>
  [[nodiscard]] decltype(auto) get([[maybe_unused]] const entity_type entt) const {
    return registry_.template get<Type...>(entt);
  }

  /*! @copydoc get */
  template <typename... Type>
  [[nodiscard]] decltype(auto) get([[maybe_unused]] const entity_type entt) {
    return registry_.template get<Type...>(entt);
  }

  template <typename Type, typename... Args>
  [[nodiscard]] decltype(auto) get_or_emplace(const entity_type entt, Args&&... args);

  template <typename... Type>
  [[nodiscard]] auto try_get([[maybe_unused]] const entity_type entt) const {
    return registry_.template try_get<Type...>(entt);
  }

  template <typename... Type>
  [[nodiscard]] auto try_get([[maybe_unused]] const entity_type entt) {
    return registry_.template try_get<Type...>(entt);
  }

  template <typename... Type>
  void clear() {
    return registry_.template clear<Type...>();
  }

  [[nodiscard]] bool orphan(const entity_type entt) const {
    return registry_.orphan(entt);
  }

  template <typename Type, typename... Other, typename... Exclude>
  [[nodiscard]] auto view(entt::exclude_t<Exclude...> = entt::exclude_t{}) {
    return registry_.template view<Type, Other...>(entt::exclude_t<Exclude...>{});
  }

  template <typename Type, typename... Other, typename... Exclude>
  [[nodiscard]] auto view(entt::exclude_t<Exclude...> = entt::exclude_t{}) const {
    return registry_.template view<Type, Other...>(entt::exclude_t<Exclude...>{});
  }

  template <typename... Owned, typename... Get, typename... Exclude>
  auto group(entt::get_t<Get...> = entt::get_t{}, entt::exclude_t<Exclude...> = entt::exclude_t{}) {
    return registry_.template group<Owned...>(entt::get_t<Get...>{}, entt::exclude_t<Exclude...>{});
  }

  template <typename Type, typename... ArgsT>
  decltype(auto) get_or_emplace(ArgsT&&... args) {
    return get_or_emplace<Type>(global_entity_, std::forward<ArgsT>(args)...);
  }

  template <typename Type>
  bool contains() const {
    return registry_.template any_of<Type>(global_entity_);
  }

  template <typename Type>
  decltype(auto) get() const {
    return registry_.template get<Type>(global_entity_);
  }

  template <typename Type>
  decltype(auto) get() {
    return registry_.template get<Type>(global_entity_);
  }

  void delete_later(const entity_type entt);

  size_t index(const entity_type entt) const {
    return registry_.template storage<entity_type>()->index(entt);
  }

  entity_type at(size_t pos) const {
    return registry_.template storage<entity_type>()->data()[pos];
  }

  entt::registry& registry() {
    return registry_;
  }

  const entt::registry& registry() const {
    return registry_;
  }

  entt_ext::entity get_global_entity() const {
    return global_entity_;
  }

  bool is_loading() const {
    return is_loading_;
  }

  template <typename Type, typename Compare, typename Sort = entt::std_sort, typename... Args>
  void sort(Compare compare, Sort algo = Sort{}, Args&&... args) {
    registry_.template sort<Type>(compare, algo, std::forward<Args>(args)...);
  }

  template <typename To, typename From>
  void sort() {
    registry_.template sort<To, From>();
  }

  template <typename Type, typename It>
  void sort_as(It first, It last) {
    auto& storage = registry_.template storage<Type>();
    storage.sort_as(first, last);
  }

  [[nodiscard]] bool valid(const entity_type entt) const {
    return registry_.valid(entt);
  }

  template <typename Type>
  void dispatch_on_construct(registry_type& registry, entt_ext::entity entt) {
    for (auto [_, component] : registry.template view<entt_ext::component_observer<Type, self_type>>().each()) {
      component.dispatch_on_construct(*this, entt);
    }
  }

  template <typename Type>
  void dispatch_on_destroy(registry_type& registry, entt_ext::entity entt) {
    for (auto [_, component] : registry.template view<entt_ext::component_observer<Type, self_type>>().each()) {
      component.dispatch_on_destroy(*this, entt);
    }
  }

  template <typename Type>
  void dispatch_on_update(registry_type& registry, entt_ext::entity entt) {
    for (auto [_, component] : registry.template view<entt_ext::component_observer<Type, self_type>>().each()) {
      component.dispatch_on_update(*this, entt);
    }
  }

  template <typename ParentType, typename ChildType, typename... ArgsT>
  decltype(auto) emplace_child(entt_ext::entity prnt, entt_ext::entity chld, ArgsT&&... args) {
    get_or_emplace<children<ChildType>>(prnt).insert(chld);

    emplace<parent<ParentType>>(chld, prnt);
    return emplace<ChildType>(chld, std::forward<ArgsT>(args)...);
  }

  template <typename ParentType, typename ChildType, typename... ArgsT>
  decltype(auto) emplace_or_replace_child(entt_ext::entity prnt, entt_ext::entity chld, ArgsT&&... args) {
    get_or_emplace<children<ChildType>>(prnt).insert(chld);

    emplace_or_replace<parent<ParentType>>(chld, prnt);
    return emplace_or_replace<ChildType>(chld, std::forward<ArgsT>(args)...);
  }

  template <typename ParentType, typename ChildType, typename... ArgsT>
  decltype(auto) emplace_child(entt_ext::entity prnt, ArgsT&&... args) {
    return emplace_child<ParentType, ChildType>(prnt, create(), std::forward<ArgsT>(args)...);
  }

  template <typename ParentType, typename ChildType>
  void assign_child(entt_ext::entity prnt, entt_ext::entity chld) {
    get_or_emplace<children<ChildType>>(prnt).insert(chld);

    if (auto parent_ptr = try_get<parent<ParentType>>(chld); parent_ptr) {
      if (parent_ptr->entity == prnt) {
        return;
      }
      if (auto prev_relationship = try_get<children<ChildType>>(parent_ptr->entity); prev_relationship && prev_relationship->contains(chld)) {
        prev_relationship->erase(chld);
      }
      parent_ptr->entity = prnt;
    } else {
      emplace<parent<ParentType>>(chld, prnt);
    }
  }

  template <typename Type, typename... Others, typename FuncT>
  void each_child(entt_ext::entity prnt, children<Type>& chldrn, FuncT&& func) {
    auto v = view<Type, Others...>();

    for (auto entity : chldrn) {
      if (v.contains(entity)) {
        std::apply(func, std::tuple_cat(std::tie(*this, prnt, entity), v.get(entity)));
      }
    }
  }

  template <typename Type, typename... Others, typename FuncT>
  void each_child(entt_ext::entity prnt, FuncT&& func) {
    if (auto children_ptr = try_get<children<Type>>(prnt); children_ptr) {
      each_child<Type, Others...>(prnt, *children_ptr, std::move(func));
    }
  }

  template <typename Type, typename... Others>
  children_range<self_type, Type, Others...> children_view(entt_ext::entity prnt) {
    return children_range<self_type, Type, Others...>(*this, get_or_emplace<children<Type>>(prnt));
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

  // Defers a synchronous function to be executed on the ECS command queue
  inline void defer(std::function<void(ecs&)> func) {
    asio::co_spawn(
        concurrent_io_context(),
        [this, func = std::move(func)]() -> asio::awaitable<void> {
          deferred_command cmd{.operation      = deferred_command::op_type::custom,
                               .entity         = entt_ext::null,
                               .component_type = 0,
                               .executor       = [func = std::move(func)](ecs& ecs_ref) mutable -> asio::awaitable<void> {
                                 func(ecs_ref);
                                 co_return;
                               }};

          co_await command_channel_.async_send(boost::system::error_code{}, std::move(cmd), asio::use_awaitable);
          co_return;
        },
        asio::detached);
  }

  // Defers an async function to be executed on the ECS command queue
  inline void defer_async(std::function<asio::awaitable<void>(ecs&)> func) {
    asio::co_spawn(
        concurrent_io_context(),
        [this, func = std::move(func)]() -> asio::awaitable<void> {
          deferred_command cmd{.operation      = deferred_command::op_type::custom,
                               .entity         = entt_ext::null,
                               .component_type = 0,
                               .executor       = std::move(func)};

          co_await command_channel_.async_send(boost::system::error_code{}, std::move(cmd), asio::use_awaitable);
          co_return;
        },
        asio::detached);
  }

  void run(int timeout_ms = 10, size_t concurrency = std::thread::hardware_concurrency());
  void stop();
  auto main_io_context() -> asio::io_context&;
  auto main_io_context() const -> const asio::io_context&;
  auto concurrent_io_context() -> asio::io_context&;

  // ============= Deferred Operations API ============

  // Deferred structural operations
  entity_type create_deferred();
  auto        destroy_deferred(const entity_type entt) -> asio::awaitable<void>;

  template <typename Type, typename... Args>
  auto emplace_deferred(const entity_type entt, Args&&... args) -> asio::awaitable<void>;

  template <typename Type, typename... Args>
  auto emplace_or_replace_deferred(const entity_type entt, Args&&... args) -> asio::awaitable<void>;

  template <typename Type, typename... Args>
  auto emplace_if_not_exists_deferred(const entity_type entt, Args&&... args) -> asio::awaitable<void>;

  template <typename Type, typename... Args>
  auto replace_deferred(const entity_type entt, Args&&... args) -> asio::awaitable<void>;

  template <typename Type, typename... Other>
  auto remove_deferred(const entity_type entt) -> asio::awaitable<void>;

  template <typename Type, typename... Func>
  auto patch_deferred(const entity_type entt, Func&&... func) -> asio::awaitable<void>;

  template <typename... ComponentsT, typename... ExcludeT>
  auto system(entt::exclude_t<ExcludeT...>) -> system_builder<entt::get_t<ComponentsT...>, entt::exclude_t<ExcludeT...>>;

  template <typename... ComponentsT>
  auto system() -> system_builder<entt::get_t<ComponentsT...>, entt::exclude_t<>>;

  template <typename ArchiveT, typename... ComponentsT>
  asio::awaitable<void> load_snapshot(ArchiveT& ar) {
    co_return co_await asio::async_compose<decltype(asio::use_awaitable), void()>(
        [&ar, this](auto&& self) {
          asio::post(main_io_context(), [self = std::move(self), &ar, this]() mutable {
            {
              is_loading_ = true;
              entt::snapshot_loader{registry_}.get<entt_ext::entity>(ar);
              (entt::snapshot_loader{registry_}.template get<ComponentsT>(ar), ...);
              // (entt::snapshot{registry_}.template get<entt_ext::parent<ComponentsT>>(ar), ...);
              // (entt::snapshot{registry_}.template get<entt_ext::children<ComponentsT>>(ar), ...);
              is_loading_ = false;
            }
            self.complete();
          });
        },
        asio::use_awaitable);
  }

  template <typename ArchiveT, typename... ComponentsT>
  asio::awaitable<void> save_snapshot(ArchiveT& ar) const {
    co_return co_await asio::async_compose<decltype(asio::use_awaitable), void()>(
        [&ar, this](auto&& self) {
          asio::post(const_cast<asio::io_context&>(main_io_context()), [self = std::move(self), &ar, this]() mutable {
            {

              entt::snapshot{registry_}.get<entt_ext::entity>(ar);
              (entt::snapshot{registry_}.template get<ComponentsT>(ar), ...);
              // (entt::snapshot{registry_}.template get<entt_ext::parent<ComponentsT>>(ar), ...);
              // (entt::snapshot{registry_}.template get<entt_ext::children<ComponentsT>>(ar), ...);
            }
            self.complete();
          });
        },
        asio::use_awaitable);
  }

  template <typename ArchiveT, typename... ComponentsT>
  asio::awaitable<bool> merge_snapshot(ArchiveT& ar) {
    co_return co_await asio::async_compose<decltype(asio::use_awaitable), void(bool)>(
        [&ar, this](auto&& self) {
          asio::post(main_io_context(), [self = std::move(self), &ar, this]() mutable {
            {
              is_loading_ = true;

              // Load entities
              continuous_loader_.get<entt_ext::entity>(ar);

              // Load components
              (continuous_loader_.template get<ComponentsT>(ar), ...);
              // (continuous_loader_.template get<entt_ext::parent<ComponentsT>>(ar), ...);
              // (continuous_loader_.template get<entt_ext::children<ComponentsT>>(ar), ...);

              continuous_loader_.orphans();

              // Remap entity references inside components after loading
              (remap_component_entities<ComponentsT>(), ...);
              // (remap_component_entities<entt_ext::parent<ComponentsT>>(), ...);
              // (remap_component_entities<entt_ext::children<ComponentsT>>(), ...);

              is_loading_ = false;
            }
            self.complete(true);
          });
        },
        asio::use_awaitable);
  }

public:
  // Channel processor coroutine (public so systems can spawn their own processors)
  asio::awaitable<void> process_command_channel();

private:
  // Helper to remap entity references in components after loading from snapshot
  template <typename ComponentT>
  void remap_component_entities() {
    auto view = registry_.view<ComponentT>();
    for (auto entity : view) {
      auto& component = view.template get<ComponentT>(entity);

      // Try member function first
      if constexpr (requires { component.map_entities(continuous_loader_); }) {
        component.map_entities(continuous_loader_);
      }
      // Try free function (for children types)
      else if constexpr (requires { map_entities(component, continuous_loader_); }) {
        map_entities(component, continuous_loader_);
      }
    }
  }

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
  registry_type                                                     registry_;
  entt_ext::entity                                                  global_entity_ = entt_ext::null;
  continuous_loader_with_mapping<registry_type>                     continuous_loader_;
  asio::io_context                                                  main_io_context_       = asio::io_context{};
  asio::io_context                                                  concurrent_io_context_ = asio::io_context{};
  std::vector<std::function<asio::awaitable<void>(entt_ext::ecs&)>> defered_tasks_;
  bool                                                              running_         = true;
  bool                                                              is_loading_      = false;
  command_channel                                                   command_channel_ = command_channel{main_io_context(), 1000};
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
template <typename Type>
inline decltype(auto) ecs::component_observer() {

  using component_observer_type = entt_ext::component_observer<Type, self_type>;

  if (auto observer_ptr = registry_.template try_get<component_observer_type>(global_entity_); observer_ptr != nullptr) {
    return *observer_ptr;
  }

  registry_.template on_construct<Type>().template connect<&ecs::dispatch_on_construct<Type>>(*this);
  registry_.template on_destroy<Type>().template connect<&ecs::dispatch_on_destroy<Type>>(*this);
  registry_.template on_update<Type>().template connect<&ecs::dispatch_on_update<Type>>(*this);

  return registry_.template emplace<component_observer_type>(global_entity_, *this, global_entity_);

  // using component_observer_type = entt_ext::component_observer<Type, self_type>;

  // auto view = registry_.template view<component_observer_type>();
  // if (view.begin() == view.end()) {
  //   // spdlog::debug("Registering component {}", type_name<Type>());
  //   auto           entity    = create();
  //   decltype(auto) component = registry_.template emplace<component_observer_type>(entity, *this, entity);
  //   registry_.template on_construct<Type>().template connect<&ecs::dispatch_on_construct<Type>>(*this);
  //   registry_.template on_destroy<Type>().template connect<&ecs::dispatch_on_destroy<Type>>(*this);
  //   registry_.template on_update<Type>().template connect<&ecs::dispatch_on_update<Type>>(*this);

  //   return component;
  // }
  // return view.template get<component_observer_type>(view.front());
}

template <typename Type, typename... Args>
inline decltype(auto) ecs::emplace(const entity_type entt, Args&&... args) {
  component_observer<Type>();
  return registry_.template emplace<Type>(entt, std::forward<Args>(args)...);
}

template <typename Type, typename It>
inline void ecs::insert(It first, It last, const Type& value) {
  component_observer<Type>();
  return registry_.template insert<Type>(std::move(first), std::move(last), value);
}

template <typename Type, typename EIt, typename CIt, typename>
inline void ecs::insert(EIt first, EIt last, CIt from) {
  component_observer<Type>();
  registry_.template insert<Type>(first, last, from);
}

template <typename Type, typename... Args>
inline decltype(auto) ecs::emplace_or_replace(const entity_type entt, Args&&... args) {
  component_observer<Type>();

  return registry_.template emplace_or_replace<Type>(entt, std::forward<Args>(args)...);
}

template <typename Type, typename... Args>
inline decltype(auto) ecs::get_or_emplace(const entity_type entt, Args&&... args) {
  component_observer<Type>();
  return registry_.template get_or_emplace<Type>(entt, std::forward<Args>(args)...);
}
inline void ecs::delete_later(const entity_type entt) {
  return emplace<entt_ext::delete_later>(entt);
}

// ============= Deferred Operations Template Implementations =============

template <typename Type, typename... Args>
inline auto ecs::emplace_deferred(const entity_type entt, Args&&... args) -> asio::awaitable<void> {
  auto args_tuple = std::make_tuple(std::forward<Args>(args)...);

  deferred_command cmd{.operation      = deferred_command::op_type::emplace,
                       .entity         = entt,
                       .component_type = entt::type_hash<Type>::value(),
                       .executor       = [entt, args_tuple = std::move(args_tuple)](ecs& ecs_ref) mutable -> asio::awaitable<void> {
                         std::apply(
                             [&](auto&&... args) {
                               ecs_ref.template emplace<Type>(entt, std::forward<decltype(args)>(args)...);
                             },
                             std::move(args_tuple));
                         co_return;
                       }};

  co_await command_channel_.async_send(boost::system::error_code{}, std::move(cmd), asio::use_awaitable);

  co_return;
}

template <typename Type, typename... Args>
inline auto ecs::emplace_or_replace_deferred(const entity_type entt, Args&&... args) -> asio::awaitable<void> {
  // Always defer - this is the explicit _deferred API
  auto args_tuple = std::make_tuple(std::forward<Args>(args)...);

  deferred_command cmd{.operation      = deferred_command::op_type::emplace,
                       .entity         = entt,
                       .component_type = entt::type_hash<Type>::value(),
                       .executor       = [entt, args_tuple = std::move(args_tuple)](ecs& ecs_ref) mutable {
                         std::apply(
                             [&](auto&&... args) {
                               ecs_ref.template emplace_or_replace<Type>(entt, std::forward<decltype(args)>(args)...);
                             },
                             std::move(args_tuple));
                         co_return;
                       }};

  co_await command_channel_.async_send(boost::system::error_code{}, std::move(cmd), asio::use_awaitable);
  co_return;
}

template <typename Type, typename... Args>
inline auto ecs::emplace_if_not_exists_deferred(const entity_type entt, Args&&... args) -> asio::awaitable<void> {
  auto args_tuple = std::make_tuple(std::forward<Args>(args)...);

  deferred_command cmd{.operation      = deferred_command::op_type::emplace_if_not_exists,
                       .entity         = entt,
                       .component_type = entt::type_hash<Type>::value(),
                       .executor       = [entt, args_tuple = std::move(args_tuple)](ecs& ecs_ref) mutable -> asio::awaitable<void> {
                         std::apply(
                             [&](auto&&... args) {
                               ecs_ref.template emplace_if_not_exists<Type>(entt, std::forward<decltype(args)>(args)...);
                             },
                             std::move(args_tuple));
                         co_return;
                       }};

  co_await command_channel_.async_send(boost::system::error_code{}, std::move(cmd), asio::use_awaitable);
  co_return;
}

template <typename Type, typename... Args>
inline auto ecs::replace_deferred(const entity_type entt, Args&&... args) -> asio::awaitable<void> {

  auto args_tuple = std::make_tuple(std::forward<Args>(args)...);

  deferred_command cmd{.operation      = deferred_command::op_type::emplace_or_replace,
                       .entity         = entt,
                       .component_type = entt::type_hash<Type>::value(),
                       .executor       = [entt, args_tuple = std::move(args_tuple)](ecs& ecs_ref) mutable -> asio::awaitable<void> {
                         std::apply(
                             [&](auto&&... args) {
                               ecs_ref.template replace<Type>(entt, std::forward<decltype(args)>(args)...);
                             },
                             std::move(args_tuple));
                         co_return;
                       }};

  co_await command_channel_.async_send(boost::system::error_code{}, std::move(cmd), asio::use_awaitable);

  co_return;
}

template <typename Type, typename... Other>
inline auto ecs::remove_deferred(const entity_type entt) -> asio::awaitable<void> {
  deferred_command cmd{.operation      = deferred_command::op_type::remove,
                       .entity         = entt,
                       .component_type = entt::type_hash<Type>::value(),
                       .executor       = [entt](ecs& ecs_ref) -> asio::awaitable<void> {
                         ecs_ref.template remove<Type, Other...>(entt);
                         co_return;
                       }};

  co_await command_channel_.async_send(boost::system::error_code{}, std::move(cmd), asio::use_awaitable);

  co_return;
}

template <typename Type, typename... Func>
inline auto ecs::patch_deferred(const entity_type entt, Func&&... func) -> asio::awaitable<void> {

  auto funcs_tuple = std::make_tuple(std::forward<Func>(func)...);

  deferred_command cmd{.operation      = deferred_command::op_type::patch,
                       .entity         = entt,
                       .component_type = entt::type_hash<Type>::value(),
                       .executor       = [entt, funcs_tuple = std::move(funcs_tuple)](ecs& ecs_ref) -> asio::awaitable<void> {
                         std::apply(
                             [&](auto&&... funcs) {
                               ecs_ref.template patch<Type>(entt, std::forward<decltype(funcs)>(funcs)...);
                             },
                             std::move(funcs_tuple));
                         co_return;
                       }};

  co_await command_channel_.async_send(boost::system::error_code{}, std::move(cmd), asio::use_awaitable);

  co_return;
}

inline auto ecs::destroy_deferred(const entity_type entt) -> asio::awaitable<void> {
  deferred_command cmd{.operation = deferred_command::op_type::destroy, .entity = entt, .executor = [entt](ecs& ecs_ref) -> asio::awaitable<void> {
                         ecs_ref.destroy(entt);
                         co_return;
                       }};
  co_await command_channel_.async_send(boost::system::error_code{}, std::move(cmd), asio::use_awaitable);
  co_return;
}
} // namespace entt_ext