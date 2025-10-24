#pragma once

#include "core.hpp"

#include <entt/core/type_info.hpp>
#include <entt/entity/entity.hpp>
#include <entt/entity/snapshot.hpp>

#include <unordered_map>

namespace entt_ext {

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
class continuous_loader_with_mapping : public entt::basic_continuous_loader<Registry> {
  using base_type   = entt::basic_continuous_loader<Registry>;
  using entity_type = typename Registry::entity_type;
  using traits_type = entt::entt_traits<entity_type>;

  // The internal mapping type from entt::basic_continuous_loader
  // Maps: base_entity_id -> pair(server_entity_with_version, client_entity_with_version)
  using remloc_type = entt::dense_map<typename traits_type::entity_type, std::pair<entity_type, entity_type>>;

public:
  using base_type::base_type; // Inherit constructors

  /**
   * @brief Extract all server->client entity mappings
   *
   * This efficiently returns all mappings by directly accessing the internal
   * remloc table. No iteration through entity ID space is needed.
   *
   * @return Map of server entity IDs to client entity IDs
   */
  [[nodiscard]] std::unordered_map<entity_type, entity_type> extract_mappings() const {
    std::unordered_map<entity_type, entity_type> result;

    // Access the internal remloc map
    const auto& mappings = get_remloc();

    // Extract server -> client mappings from the remloc table
    for (const auto& [base_entity, pair] : mappings) {
      const auto& [server_entity, client_entity] = pair;
      result[server_entity]                      = client_entity;
    }

    return result;
  }

  /**
   * @brief Get direct access to the internal remloc mapping
   *
   * This provides read-only access to the internal entity mapping table.
   *
   * @return Reference to the remloc map
   */
  [[nodiscard]] const remloc_type& get_remloc() const {
    // Access private member using memory layout knowledge
    // The basic_continuous_loader has the following layout:
    // 1. remloc (dense_map)
    // 2. reg (Registry*)

    // Use an accessor struct to reach private members
    struct accessor {
      remloc_type remloc;
      Registry*   reg;
    };

    // Cast this to accessor to get the remloc
    return reinterpret_cast<const accessor*>(this)->remloc;
  }
};

// Type alias for common case
using continuous_loader_with_mapping_t = continuous_loader_with_mapping<entt::registry>;

} // namespace entt_ext
