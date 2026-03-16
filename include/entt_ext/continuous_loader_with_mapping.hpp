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
 * Uses the full entity identifier (ID + version) as the mapping key,
 * so recycled entity IDs with different versions get separate live entities
 * instead of colliding.
 *
 * The internal `remloc` map stores: remote_entity -> local_entity
 * We expose this through accessor methods.
 */
template <typename Registry>
class continuous_loader_with_mapping {
  static_assert(!std::is_const_v<Registry>, "Non-const registry type required");
  using traits_type = entt::entt_traits<typename Registry::entity_type>;

  void restore(typename Registry::entity_type entt) {
    if (auto it = remloc_.find(entt); it != remloc_.end()) {
      // Exact match (same ID + version) — reuse if live entity is still valid
      if (!reg->valid(it->second)) {
        locrem_.erase(it->second);
        const auto local = reg->create();
        it->second       = local;
        locrem_.insert_or_assign(local, entt);
      }
    } else {
      // New entity (or recycled ID with different version) — create a new live entity
      const auto local = reg->create();
      remloc_.insert_or_assign(entt, local);
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

        // Dead entity — clean up any mapping with the same base ID (any version)
        const auto base = to_entity(entt);
        for (auto it = remloc_.begin(); it != remloc_.end();) {
          if (to_entity(it->first) == base) {
            auto local = it->second;
            if (reg->valid(local)) {
              reg->destroy(local);
            }
            locrem_.erase(local);
            it = remloc_.erase(it);
          } else {
            ++it;
          }
        }
      }
    } else {
      for (auto&& [remote, local] : remloc_) {
        storage.remove(local);
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
   * @brief Destroys loader-managed entities that have no elements.
   *
   * In case all the entities were serialized but only part of the elements
   * was saved, it could happen that some of the entities have no elements
   * once restored.<br/>
   * This function helps to identify and destroy those entities.
   *
   * Only entities known to this loader (i.e. created via restore()) are
   * considered, so pre-existing application entities with no components are
   * left untouched.
   *
   * @return A non-const reference to this loader.
   */
  continuous_loader_with_mapping& orphans() {
    for (auto it = remloc_.begin(); it != remloc_.end();) {
      auto local = it->second;
      if (reg->valid(local) && reg->orphan(local)) {
        reg->destroy(local);
        locrem_.erase(local);
        it = remloc_.erase(it);
      } else {
        ++it;
      }
    }
    return *this;
  }

  /**
   * @brief Tests if a loader knows about a given entity.
   * @param entt A valid identifier.
   * @return True if `entity` is managed by the loader, false otherwise.
   */
  [[nodiscard]] bool contains(entity_type entt) const noexcept {
    return remloc_.contains(entt);
  }

  /**
   * @brief Returns the local identifier to which a remote entity refers.
   * @param entt A valid remote identifier.
   * @return The local identifier if any, the null entity otherwise.
   */
  [[nodiscard]] entity_type to_local(entity_type entt) const noexcept {
    if (const auto it = remloc_.find(entt); it != remloc_.cend()) {
      return it->second;
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
   * @brief Tests if a loader knows about a given local entity.
   * @param entt A valid local identifier.
   * @return True if the local entity is managed by the loader, false otherwise.
   */
  [[nodiscard]] bool contains_local(entity_type entt) const noexcept {
    return locrem_.find(entt) != locrem_.cend();
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
    remloc_.insert_or_assign(remote, local);
    locrem_.insert_or_assign(local, remote);
  }

  /**
   * @brief Manually remove a remote-to-local entity mapping.
   *
   * This allows external code to remove entity mappings without going
   * through the deserialization process. The remote entity is provided from
   * outside the loader.
   *
   * @param remote The remote (server) entity identifier.
   */
  void remove_mapping_by_remote(entity_type remote) {
    if (const auto it = remloc_.find(remote); it != remloc_.cend()) {
      locrem_.erase(it->second);
      remloc_.erase(it);
    }
  }
  /**
   * @brief Manually remove a remote-to-local entity mapping.
   *
   * This allows external code to remove entity mappings without going
   * through the deserialization process. The local entity is provided from
   * outside the loader.
   *
   * @param local The local (client) entity identifier.
   */
  void remove_mapping_by_local(entity_type local) {
    if (const auto it = locrem_.find(local); it != locrem_.cend()) {
      remloc_.erase(it->second);
      locrem_.erase(it);
    }
  }

private:
  entt::dense_map<entity_type, entity_type> remloc_; // remote (archived) entity → local (live) entity
  entt::dense_map<entity_type, entity_type> locrem_; // local (live) entity → remote (archived) entity
  registry_type*                            reg;
};

} // namespace entt_ext
