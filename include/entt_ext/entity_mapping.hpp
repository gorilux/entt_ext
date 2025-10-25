#pragma once

#include "ecs.hpp"

#include <cereal/archives/portable_binary.hpp>
#include <cereal/types/unordered_map.hpp>

#include <optional>
#include <unordered_map>

namespace entt_ext::sync {

// Entity mapping between client and server
// struct entity_mapping {
//   std::unordered_map<entity, entity> client_to_server; // Maps client entity ID to server entity ID
//   std::unordered_map<entity, entity> server_to_client; // Maps server entity ID to client entity ID

//   template <typename Archive>
//   void serialize(Archive& archive) {
//     archive(client_to_server, server_to_client);
//   }

//   // Add a bidirectional mapping
//   void add_mapping(entity client_entity, entity server_entity) {
//     client_to_server[client_entity] = server_entity;
//     server_to_client[server_entity] = client_entity;
//   }

//   // Remove a mapping (both directions)
//   void remove_mapping(entity client_entity) {
//     auto it = client_to_server.find(client_entity);
//     if (it != client_to_server.end()) {
//       server_to_client.erase(it->second);
//       client_to_server.erase(it);
//     }
//   }

//   void remove_server_mapping(entity server_entity) {
//     auto it = server_to_client.find(server_entity);
//     if (it != server_to_client.end()) {
//       client_to_server.erase(it->second);
//       server_to_client.erase(it);
//     }
//   }

//   // Get server entity from client entity
//   std::optional<entity> get_server_entity(entity client_entity) const {
//     auto it = client_to_server.find(client_entity);
//     return it != client_to_server.end() ? std::make_optional(it->second) : std::nullopt;
//   }

//   // Get client entity from server entity
//   std::optional<entity> get_client_entity(entity server_entity) const {
//     auto it = server_to_client.find(server_entity);
//     return it != server_to_client.end() ? std::make_optional(it->second) : std::nullopt;
//   }

//   // Check if client entity is mapped
//   bool has_client_entity(entity client_entity) const {
//     return client_to_server.find(client_entity) != client_to_server.end();
//   }

//   // Check if server entity is mapped
//   bool has_server_entity(entity server_entity) const {
//     return server_to_client.find(server_entity) != server_to_client.end();
//   }

//   // Clear all mappings
//   void clear() {
//     client_to_server.clear();
//     server_to_client.clear();
//   }

//   // Get all client entities
//   std::vector<entity> get_client_entities() const {
//     std::vector<entity> entities;
//     entities.reserve(client_to_server.size());
//     for (const auto& [client_entity, _] : client_to_server) {
//       entities.push_back(client_entity);
//     }
//     return entities;
//   }

//   // Get all server entities
//   std::vector<entity> get_server_entities() const {
//     std::vector<entity> entities;
//     entities.reserve(server_to_client.size());
//     for (const auto& [server_entity, _] : server_to_client) {
//       entities.push_back(server_entity);
//     }
//     return entities;
//   }

//   size_t size() const {
//     return client_to_server.size();
//   }

//   bool empty() const {
//     return client_to_server.empty();
//   }
// };

// // Component to store entity mapping in ECS
// struct entity_mapping_component {
//   entity_mapping mapping;

//   template <typename Archive>
//   void serialize(Archive& archive) {
//     archive(mapping);
//   }
// };

} // namespace entt_ext::sync
