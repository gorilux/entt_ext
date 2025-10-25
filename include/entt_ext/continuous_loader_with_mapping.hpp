#pragma once

#include "core.hpp"

#include <entt/core/type_info.hpp>
#include <entt/entity/entity.hpp>

#include <entt/container/dense_map.hpp>
#include <entt/core/type_traits.hpp>

namespace entt_ext {

namespace internal {
template <typename Registry>
void orphans(Registry& registry) {
  auto& storage = registry.template storage<typename Registry::entity_type>();

  for (auto entt : storage) {
    if (registry.orphan(entt)) {
      storage.erase(entt);
    }
  }
}
} // namespace internal

/**
 * @brief Enhanced continuous loader that exposes entity mappings
 *
 * This class inherits from entt::basic_continuous_loader and provides direct
 * access to the internal entity mapping table, allowing efficient extraction
 * of all server->client entity ID mappings without iteration.
 *
 * The internal `remloc` map stores: base_entity_id -> pair(server_entity, client_entity)
 * We expose this through an accessor method to avoid modifying entt's source.
 */
template <typename Registry>
class continuous_loader_with_mapping {
  static_assert(!std::is_const_v<Registry>, "Non-const registry type required");
  using traits_type = entt::entt_traits<typename Registry::entity_type>;

  void restore(typename Registry::entity_type entt) {
    if (const auto entity = to_entity(entt); remloc_.contains(entity) && remloc_[entity].first == entt) {
      if (!reg->valid(remloc_[entity].second)) {
        const auto local       = reg->create();
        remloc_[entity].second = local;
        locrem_.insert_or_assign(local, entt);
      }
    } else {
      const auto local = reg->create();
      remloc_.insert_or_assign(entity, std::make_pair(entt, local));
      locrem_.insert_or_assign(local, entt);
    }
  }

  template <typename Container>
  auto update(int, Container& container) -> decltype(typename Container::mapped_type{}, void()) {
    // map like container
    Container other;

    for (auto&& pair : container) {
      using first_type  = std::remove_const_t<typename std::decay_t<decltype(pair)>::first_type>;
      using second_type = typename std::decay_t<decltype(pair)>::second_type;

      if constexpr (std::is_same_v<first_type, entity_type> && std::is_same_v<second_type, entity_type>) {
        other.emplace(map(pair.first), map(pair.second));
      } else if constexpr (std::is_same_v<first_type, entity_type>) {
        other.emplace(map(pair.first), std::move(pair.second));
      } else {
        static_assert(std::is_same_v<second_type, entity_type>, "Neither the key nor the value are of entity type");
        other.emplace(std::move(pair.first), map(pair.second));
      }
    }

    using std::swap;
    swap(container, other);
  }

  template <typename Container>
  auto update(char, Container& container) -> decltype(typename Container::value_type{}, void()) {
    // vector like container
    static_assert(std::is_same_v<typename Container::value_type, entity_type>, "Invalid value type");

    for (auto&& entt : container) {
      entt = map(entt);
    }
  }

  template <typename Component, typename Other, typename Member>
  void update([[maybe_unused]] Component& instance, [[maybe_unused]] Member Other::* member) {
    if constexpr (!std::is_same_v<Component, Other>) {
      return;
    } else if constexpr (std::is_same_v<Member, entity_type>) {
      instance.*member = map(instance.*member);
    } else {
      // maybe a container? let's try...
      update(0, instance.*member);
    }
  }

public:
  /*! Basic registry type. */
  using registry_type = Registry;
  /*! @brief Underlying entity identifier. */
  using entity_type = typename registry_type::entity_type;

  /**
   * @brief Constructs an instance that is bound to a given registry.
   * @param source A valid reference to a registry.
   */
  continuous_loader_with_mapping(registry_type& source) noexcept
    : remloc_{source.get_allocator()}
    , locrem_{source.get_allocator()}
    , reg{&source} {
  }

  /*! @brief Default copy constructor, deleted on purpose. */
  continuous_loader_with_mapping(const continuous_loader_with_mapping&) = delete;

  /*! @brief Default move constructor. */
  continuous_loader_with_mapping(continuous_loader_with_mapping&&) noexcept = default;

  /*! @brief Default destructor. */
  ~continuous_loader_with_mapping() = default;

  /**
   * @brief Default copy assignment operator, deleted on purpose.
   * @return This loader.
   */
  continuous_loader_with_mapping& operator=(const continuous_loader_with_mapping&) = delete;

  /**
   * @brief Default move assignment operator.
   * @return This loader.
   */
  continuous_loader_with_mapping& operator=(continuous_loader_with_mapping&&) noexcept = default;

  /**
   * @brief Restores all elements of a type with associated identifiers.
   *
   * It creates local counterparts for remote elements as needed.<br/>
   * Members are either data members of type entity_type or containers of
   * entities. In both cases, a loader visits them and replaces entities with
   * their local counterpart.
   *
   * @tparam Type Type of elements to restore.
   * @tparam Archive Type of input archive.
   * @param archive A valid reference to an input archive.
   * @param id Optional name used to map the storage within the registry.
   * @return A valid loader to continue restoring data.
   */
  template <typename Type, typename Archive>
  continuous_loader_with_mapping& get(Archive& archive, const id_type id = entt::type_hash<Type>::value()) {
    auto&                             storage = reg->template storage<Type>(id);
    typename traits_type::entity_type length{};
    entity_type                       entt{null};

    archive(length);

    if constexpr (std::is_same_v<Type, entity_type>) {
      typename traits_type::entity_type in_use{};

      storage.reserve(length);
      archive(in_use);

      for (std::size_t pos{}; pos < in_use; ++pos) {
        archive(entt);
        restore(entt);
      }

      for (std::size_t pos = in_use; pos < length; ++pos) {
        archive(entt);

        if (const auto entity = to_entity(entt); remloc_.contains(entity)) {
          const auto local = remloc_[entity].second;
          if (reg->valid(local)) {
            reg->destroy(local);
          }
          locrem_.erase(local);
          remloc_.erase(entity);
        }
      }
    } else {
      for (auto&& ref : remloc_) {
        storage.remove(ref.second.second);
      }

      while (length--) {
        if (archive(entt); entt != null) {
          restore(entt);

          if constexpr (std::tuple_size_v<decltype(storage.get_as_tuple({}))> == 0u) {
            storage.emplace(map(entt));
          } else {
            Type elem{};
            archive(elem);
            storage.emplace(map(entt), std::move(elem));
          }
        }
      }
    }

    return *this;
  }

  /**
   * @brief Destroys those entities that have no elements.
   *
   * In case all the entities were serialized but only part of the elements
   * was saved, it could happen that some of the entities have no elements
   * once restored.<br/>
   * This function helps to identify and destroy those entities.
   *
   * @return A non-const reference to this loader.
   */
  continuous_loader_with_mapping& orphans() {
    internal::orphans(*reg);
    return *this;
  }

  /**
   * @brief Tests if a loader knows about a given entity.
   * @param entt A valid identifier.
   * @return True if `entity` is managed by the loader, false otherwise.
   */
  [[nodiscard]] bool contains(entity_type entt) const noexcept {
    const auto it = remloc_.find(to_entity(entt));
    return it != remloc_.cend() && it->second.first == entt;
  }

  /**
   * @brief Returns the local identifier to which a remote entity refers.
   * @param entt A valid remote identifier.
   * @return The local identifier if any, the null entity otherwise.
   */
  [[nodiscard]] entity_type to_local(entity_type entt) const noexcept {
    if (const auto it = remloc_.find(to_entity(entt)); it != remloc_.cend() && it->second.first == entt) {
      return it->second.second;
    }

    return null;
  }

  /**
   * @brief Returns the local identifier to which a remote entity refers.
   * @param entt A valid remote identifier.
   * @return The local identifier if any, the null entity otherwise.
   * @note Alias for to_local() for backward compatibility.
   */
  [[nodiscard]] entity_type map(entity_type entt) const noexcept {
    return to_local(entt);
  }

  /**
   * @brief Returns the remote identifier to which a local entity refers.
   * @param entt A valid local identifier.
   * @return The remote identifier if any, the null entity otherwise.
   */
  [[nodiscard]] entity_type to_remote(entity_type entt) const noexcept {
    if (const auto it = locrem_.find(entt); it != locrem_.cend()) {
      return it->second;
    }

    return null;
  }

  /**
   * @brief Manually insert a remote-to-local entity mapping.
   *
   * This allows external code to establish entity mappings without going
   * through the deserialization process. Both the remote and local entities
   * are provided from outside the loader.
   *
   * @param remote The remote (server) entity identifier.
   * @param local The local (client) entity identifier.
   */
  void insert_mapping(entity_type remote, entity_type local) {
    const auto entity = to_entity(remote);
    remloc_.insert_or_assign(entity, std::make_pair(remote, local));
    locrem_.insert_or_assign(local, remote);
  }

private:
  entt::dense_map<typename traits_type::entity_type, std::pair<entity_type, entity_type>> remloc_;
  entt::dense_map<entity_type, entity_type>                                               locrem_;
  registry_type*                                                                          reg;
};

} // namespace entt_ext
