#pragma once

#include "core.hpp"
#include "ecs_fwd.hpp"

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
class component_observer {

  using handler_type       = std::function<void(EcsT&, entt_ext::entity, Type&)>;
  using async_handler_type = std::function<asio::awaitable<void>(EcsT&, entt_ext::entity, Type&)>;

public:
  component_observer(EcsT& ecs, entt_ext::entity entt) {
  }

  ~component_observer() = default;

  std::string_view name() const {
    return type_name<Type>();
  }

  template <typename FuncT>
  handler_id on_construct(FuncT&& func) {
    auto id = next_handler_id++;
    if constexpr (std::is_same_v<std::invoke_result_t<FuncT, EcsT&, entt_ext::entity, Type&>, asio::awaitable<void>>) {
      on_construct_async_handlers.emplace(id, func);
    } else {
      on_construct_handlers.emplace(id, func);
    }
    return id;
  }

  template <typename FuncT>
  handler_id on_destroy(FuncT&& func) {
    auto id = next_handler_id++;
    if constexpr (std::is_same_v<std::invoke_result_t<FuncT, EcsT&, entt_ext::entity, Type&>, asio::awaitable<void>>) {
      on_destroy_async_handlers.emplace(id, func);
    } else {
      on_destroy_handlers.emplace(id, func);
    }
    return id;
  }

  template <typename FuncT>
  handler_id on_update(FuncT&& func) {
    auto id = next_handler_id++;
    if constexpr (std::is_same_v<std::invoke_result_t<FuncT, EcsT&, entt_ext::entity, Type&>, asio::awaitable<void>>) {
      on_update_async_handlers.emplace(id, func);
    } else {
      on_update_handlers.emplace(id, func);
    }
    return id;
  }

  bool on_construct_disconnect(handler_id id) {
    if (auto it = on_construct_handlers.find(id); it != on_construct_handlers.end()) {
      on_construct_handlers.erase(it->first);
      return true;
    }
    if (auto it = on_construct_async_handlers.find(id); it != on_construct_async_handlers.end()) {
      on_construct_async_handlers.erase(it->first);
      return true;
    }
    return false;
  }

  bool on_destroy_disconnect(handler_id id) {
    if (auto it = on_destroy_handlers.find(id); it != on_destroy_handlers.end()) {
      on_destroy_handlers.erase(it->first);
      return true;
    }
    if (auto it = on_destroy_async_handlers.find(id); it != on_destroy_async_handlers.end()) {
      on_destroy_async_handlers.erase(it->first);
      return true;
    }
    return false;
  }

  bool on_update_disconnect(handler_id id) {
    if (auto it = on_update_handlers.find(id); it != on_update_handlers.end()) {
      on_update_handlers.erase(it->first);
      return true;
    }
    if (auto it = on_update_async_handlers.find(id); it != on_update_async_handlers.end()) {
      on_update_async_handlers.erase(it->first);
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

    // Handle async handlers by posting them to the ECS's IO context
    for (auto& [id, handler] : on_construct_async_handlers) {
      ecs.defer([handler, entt](EcsT& ecs_ref) -> asio::awaitable<void> {
        if constexpr (entt::component_traits<Type>::page_size > 0u) {
          co_await handler(ecs_ref, entt, ecs_ref.template get<Type>(entt));
        } else {
          Type t;
          co_await handler(ecs_ref, entt, t);
        }
      });
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

    // Handle async handlers by posting them to the ECS's IO context
    for (auto& [id, handler] : on_destroy_async_handlers) {
      ecs.defer([handler, entt](EcsT& ecs_ref) -> asio::awaitable<void> {
        if constexpr (entt::component_traits<Type>::page_size > 0u) {
          co_await handler(ecs_ref, entt, ecs_ref.template get<Type>(entt));
        } else {
          Type t;
          co_await handler(ecs_ref, entt, t);
        }
      });
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

    // Handle async handlers by posting them to the ECS's IO context
    for (auto& [id, handler] : on_update_async_handlers) {
      ecs.defer([handler, entt](EcsT& ecs_ref) -> asio::awaitable<void> {
        if constexpr (entt::component_traits<Type>::page_size > 0u) {
          co_await handler(ecs_ref, entt, ecs_ref.template get<Type>(entt));
        } else {
          Type t;
          co_await handler(ecs_ref, entt, t);
        }
      });
    }
  }

private:
  handler_id                                next_handler_id = 0;
  index_map<handler_id, handler_type>       on_construct_handlers;
  index_map<handler_id, handler_type>       on_destroy_handlers;
  index_map<handler_id, handler_type>       on_update_handlers;
  index_map<handler_id, async_handler_type> on_construct_async_handlers;
  index_map<handler_id, async_handler_type> on_destroy_async_handlers;
  index_map<handler_id, async_handler_type> on_update_async_handlers;
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
    return registry_.create(first, last);
  }

  version_type destroy(const entity_type entt);
  version_type destroy(const entity_type entt, const version_type version);

  template <typename Type, typename... ArgsT>
  decltype(auto) component_observer(ArgsT&&... args);

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
    asio::co_spawn(
        concurrent_io_context(),
        [this, func = std::move(func)]() -> asio::awaitable<void> {
          deferred_command cmd{.operation      = deferred_command::op_type::custom,
                               .entity         = entt_ext::null,
                               .component_type = 0,
                               .executor       = [func = std::move(func)](ecs& ecs_ref) mutable -> asio::awaitable<void> {
                                 if constexpr (std::is_same_v<std::invoke_result_t<FuncT, entt_ext::ecs&>, asio::awaitable<void>>) {
                                   co_await func(ecs_ref);
                                   co_return;
                                 } else {
                                   func(ecs_ref);
                                   co_return;
                                 }
                               }};

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
              entt::snapshot_loader{registry_}.get<entt_ext::entity>(ar);
              (entt::snapshot_loader{registry_}.template get<ComponentsT>(ar), ...);
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

              // Load entities
              continuous_loader_.get<entt_ext::entity>(ar);

              // Load components
              (continuous_loader_.template get<ComponentsT>(ar), ...);

              continuous_loader_.orphans();
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
template <typename Type, typename... ArgsT>
inline decltype(auto) ecs::component_observer(ArgsT&&... args) {
  using component_type = entt_ext::component_observer<Type, self_type>;

  auto view = registry_.template view<component_type>();
  if (view.begin() == view.end()) {
    // spdlog::debug("Registering component {}", type_name<Type>());
    auto           entity    = create();
    decltype(auto) component = registry_.template emplace<component_type>(entity, *this, entity, std::forward<ArgsT>(args)...);
    registry_.template on_construct<Type>().template connect<&ecs::dispatch_on_construct<Type>>(*this);
    registry_.template on_destroy<Type>().template connect<&ecs::dispatch_on_destroy<Type>>(*this);
    registry_.template on_update<Type>().template connect<&ecs::dispatch_on_update<Type>>(*this);

    return component;
  }
  return view.template get<component_type>(view.front());
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