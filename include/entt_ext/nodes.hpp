#pragma once

#include "core.hpp"

#include <cereal/cereal.hpp>
#include <spdlog/spdlog.h>
#include <entt/core/type_info.hpp>
#include <entt/container/dense_map.hpp>


#include <array>
#include <atomic>
#include <functional>
#include <set>
#include <tuple>
#include <type_traits>

namespace entt_ext {

enum class pin_type : std::uint8_t {
  undefined,
  input,
  output,
  internal
};

using pin_container_t = std::vector<entity>;

struct pin_descriptor {
  entity        id;
  entity        node_id;
  std::uint32_t index;
  std::string   label;
  pin_type      type = pin_type::undefined;

  template <typename ArchiveT>
  void serialize(ArchiveT& ar, std::uint32_t const class_version) {
    ar(id, node_id, index, label, type);
  }
};

struct link_descriptor {
  entity id;
  entity from;
  entity to;

  std::string label; // For debugging

  template <typename ArchiveT>
  void serialize(ArchiveT& ar, std::uint32_t const class_version) {
    ar(id, from, to);
  }
};

struct opaque_link {
  std::function<void(entity, entity)> create;
  std::function<void(entity, entity)> destroy;
};

struct opaque_transform {
  std::function<void(entity, entity)> create;
  std::function<void(entity, entity)> destroy;
};

template <typename T>
struct output_pin {
  entity           pin_id;
  entity           node_id;
  double_buffer<T> value;

  output_pin<T>& operator=(T const& other) {
    value = other;
    return *this;
  }
};

template <typename T>
struct input_pin {
  entity                    pin_id;
  entity                    node_id;
  std::function<const T&()> value;
};

template <typename FromT, typename ToT>
struct transform {
  entity                     input_id;
  entity                     output_id;
  std::function<void(ToT&&)> update;
};

template <typename T>
struct link {};

using pin_type_id = std::pair<entt::id_type, size_t>;

struct node_descriptor {
  entity                                  id;
  std::string                             label;
  pin_container_t                         pins;
  std::unordered_map<pin_type_id, entity> pin_type_map;

  template <typename ArchiveT>
  void save(ArchiveT& ar, std::uint32_t const class_version) const {
    ar(id, label);
    ar(pin_type_map);
  }

  template <typename ArchiveT>
  void load(ArchiveT& ar, std::uint32_t const class_version) {
    ar(id, label);
    ar(pin_type_map);
  }
};

template <typename EcsT>
class graph {
public:
  using entity_type = entity;
  using link_id     = entity_type;
  using node_id     = entity_type;
  using pin_id      = entity_type;

  graph(EcsT& ecs)
    : m_ecs(ecs) {
  }

  graph(graph const&)             = delete;
  graph& operator=(graph const&)  = delete;
  graph(graph&& other)            = delete;
  graph& operator=(graph&& other) = delete;

  void init() {
    m_ecs.template component<node_descriptor>().on_destroy([this](auto& ecs, auto e, auto& node) {
      std::vector<entity> links_to_remove{};
      std::vector<entity> pins_to_remove{};
      for (auto pin_id : node.pins) {
        // this routine is only deleting half of the pins/links...
        for (auto to : m_link_map[pin_id]) {
          if (auto link_itr = m_link_index.find(std::make_pair(pin_id, to)); link_itr != m_link_index.end()) {
            links_to_remove.push_back(link_itr->second);
          }
        }
        pins_to_remove.push_back(pin_id);
      }

      for (auto link_id : links_to_remove) {
        remove_link(link_id);
      }
      for (auto pin_id : pins_to_remove) {
        m_ecs.destroy(pin_id);
      }
    });
  }

  auto bind_node(entity id, std::string const& label) {
    return node_builder<EcsT>(m_ecs, id, label);
  }

  auto emplace_if_not_exists_node(entity id, std::string const& label) {
    if (auto node = m_ecs.template try_get<node_descriptor>(id); node) {
      if (node->label != label) {
        node->label = label;
      }
      return std::make_pair(std::ref(*node), false);
    }
    return std::make_pair(std::ref(m_ecs.template emplace<node_descriptor>(id, id, label)), true);
  }

  auto create_link(pin_id from, pin_id to) {
    if (auto link_creator = m_ecs.template try_get<opaque_link>(from); link_creator) {
      link_creator->create(from, to);
      return true;
    }
    return false;
  }

  void remove_link(link_id id) {

    if (auto link = m_ecs.template try_get<link_descriptor>(id); link) {
      m_link_map[link->from].erase(link->to);
      m_link_index.erase(std::make_pair(link->from, link->to));
      if (auto link_destroyer = m_ecs.template try_get<opaque_link>(link->from); link_destroyer) {
        link_destroyer->destroy(link->from, link->to);
      }
      spdlog::info("remove link {} {}", static_cast<size_t>(id), link->label);
      m_ecs.template remove<link_descriptor>(id);
      m_ecs.delete_later(id);
    }
  }

  template <typename ContinousLoaderT>
  void remap(ContinousLoaderT& continous_loader) {
    for (auto [e, node] : m_ecs.template view<entt_ext::node_descriptor>().each()) {
      node.id = continous_loader.map(node.id);

      for (auto& [key, value] : node.pin_type_map) {
        value = continous_loader.map(value);
      }
    }

    m_ecs.template view<entt_ext::pin_descriptor>().each([&](auto e, auto& pin) {
      pin.id      = continous_loader.map(pin.id);
      pin.node_id = continous_loader.map(pin.node_id);
    });

    decltype(m_link_map) new_link_map{};
    for (auto& kvp : m_link_map) {
      auto from = continous_loader.map(kvp.first);
      for (auto to : kvp.second) {
        new_link_map[from].insert(continous_loader.map(to));
      }
    }
    m_link_map = std::move(new_link_map);
  }

  template <typename Type, typename TagT>
  decltype(auto) bind_output(entity node_id, std::string const& label, std::uint32_t index = 0u) {
    auto& node = m_ecs.template get<node_descriptor>(node_id);
    return create_or_update_pin<Type, TagT>(node, label, pin_type::output, index);
  }

  template <typename TagT>
  decltype(auto) bind_input(entity node_id, std::string const& label, std::uint32_t index = 0u) {
    auto& node = m_ecs.template get<node_descriptor>(node_id);

    if (auto pin_id = find_pin_id<TagT>(node, index); pin_id != entt::null) {
      if (auto pin_ptr = m_ecs.template try_get<pin_descriptor>(pin_id); pin_ptr && pin_ptr->type == pin_type::input) {
        node.pins.push_back(pin_id);
        tag_pin<TagT>(node, pin_id, index);
        // if (type == pin_type::input && m_link_map[pin_id].size() > 0) {
        //   for (auto& from : m_link_map[pin_id]) {
        //     create_link_delegate(from, pin_id);
        //     // tag_pin<TagT>(node, from, index);
        //   }
        // }
        return *pin_ptr;
      }
    }

    auto pin_id = m_ecs.create();
    tag_pin<TagT>(node, pin_id, index);
    node.pins.push_back(pin_id);
    return m_ecs.template emplace<pin_descriptor>(pin_id, pin_id, node.id, index, label, pin_type::input);
  }

  template <typename Type, typename TagT>
  decltype(auto) bind_internal(entity node_id, std::string const& label, std::uint32_t index = 0u) {
    auto& node = m_ecs.template get<node_descriptor>(node_id);
    return create_or_update_pin<Type, TagT>(node, label, pin_type::internal, index);
  }

  template <typename Type>
  void create_link(entity from, entity to) {
    entity      link_id;
    auto const& pin_from = m_ecs.template get<pin_descriptor>(from);
    auto const& pin_to   = m_ecs.template get<pin_descriptor>(to);

    if (auto link_id_itr = m_link_index.find(std::make_pair(from, to)); link_id_itr != m_link_index.end()) {
      link_id = link_id_itr->second;
    } else {
      link_id = m_ecs.create();
      m_ecs.template emplace<link_descriptor>(
          link_id,
          link_id,
          from,
          to,
          std::format("{}[{}]->{}[{}]", static_cast<int>(pin_from.id), pin_from.label, static_cast<int>(pin_to.id), pin_to.label));
      m_link_index[std::make_pair(from, to)] = link_id;
    }

    m_link_map[from].insert(to);
    m_link_map[to].insert(from);

    spdlog::debug("create link {} {}",
                  static_cast<size_t>(link_id),
                  std::format("{}[{}]->{}[{}]", static_cast<int>(pin_from.id), pin_from.label, static_cast<int>(pin_to.id), pin_to.label));
    m_ecs.template emplace_if_not_exists<output_pin<Type>>(pin_from.id, pin_from.id, pin_from.node_id);
    m_ecs.template emplace_if_not_exists<input_pin<Type>>(pin_to.id, pin_to.id, pin_to.node_id, [this, output_pin_id = pin_from.id]() -> const Type& {
      return m_ecs.template get<output_pin<Type>>(output_pin_id).value.read();
    });
  }

  template <typename Type>
  void destroy_link(entity from, entity to) {
    m_link_map[from].erase(to);
    m_link_map[to].erase(from);

    if (m_link_map[from].size() == 0) {
      spdlog::debug("remove output pin {}", static_cast<size_t>(from));
      m_ecs.template remove_deferred<output_pin<Type>>(from);
    }

    // if (m_link_map[to].size() == 0) {
    spdlog::debug("remove input pin {}", static_cast<size_t>(to));
    m_ecs.template remove_deferred<input_pin<Type>>(to);
    //}
  }

  template <typename FromT, typename ToT>
  void bind_transform(entity input_id, entity output_id) {
    auto const& pin_input  = m_ecs.template get<pin_descriptor>(input_id);
    auto const& pin_output = m_ecs.template get<pin_descriptor>(output_id);

    assert(pin_input.node_id == pin_output.node_id);

    m_link_map[input_id].insert(output_id);
    m_link_map[output_id].insert(input_id);

    m_ecs.template emplace_if_not_exists<output_pin<ToT>>(pin_output.id, pin_output.id, pin_output.node_id);
    m_ecs.template emplace<transform<FromT, ToT>>(pin_input.id, pin_input.id, pin_output.id, [this, output_id = pin_output.id](ToT&& result) {
      auto& output = m_ecs.template get<output_pin<ToT>>(output_id);
      output.value = std::move(result);
    });
  }

  template <typename TagT>
  entity get_pin_id(entity node_id, std::uint32_t index = 0) {

    if (auto node_ptr = m_ecs.template try_get<node_descriptor>(node_id); node_ptr != nullptr) {
      if (auto itr = node_ptr->pin_type_map.find(std::make_pair(entt::type_hash<TagT>::value(), index)); itr != node_ptr->pin_type_map.end()) {
        return itr->second;
      }
    }
    return entt::null;
  }

  template <typename Type, typename TagT>
  output_pin<Type>* try_get_output_pin(entity node_id) {
    auto pin_id = get_pin_id<TagT>(node_id);
    return m_ecs.template try_get<output_pin<Type>>(pin_id);
  }

  template <typename Type, typename TagT>
  input_pin<Type>* try_get_pin_input(entity node_id) {
    auto pin_id = get_pin_id<TagT>(node_id);
    return m_ecs.template try_get<input_pin<Type>>(pin_id);
  }

  template <typename ArchiveT>
  void save_snapshot(ArchiveT& ar) const {
    ar(m_link_map);
  }

  template <typename ArchiveT>
  void load_snapshot(ArchiveT& ar) {
    ar(m_link_map);
  }

private:
  template <typename TagT>
  entity find_pin_id(node_descriptor const& node, size_t index) {
    if (auto itr = node.pin_type_map.find(std::make_pair(entt::type_hash<TagT>::value(), index)); itr != node.pin_type_map.end()) {
      return itr->second;
    }
    return entt::null;
  }

  template <typename TagT>
  void tag_pin(node_descriptor& node, entity pin_id, std::uint32_t index) {
    m_ecs.template emplace_if_not_exists<TagT>(pin_id);
    node.pin_type_map[std::make_pair(entt::type_hash<TagT>::value(), index)] = pin_id;
  }

  template <typename Type, typename TagT>
  pin_descriptor& create_or_update_pin(node_descriptor& node, std::string const& label, pin_type type, std::uint32_t index) {

    auto create_link_delegate = [this](entity from, entity to) mutable {
      create_link<Type>(from, to);
    };

    auto destroy_link_delegate = [this, &ecs = m_ecs](entity from, entity to) mutable {
      destroy_link<Type>(from, to);
    };

    if (auto pin_id = find_pin_id<TagT>(node, index); pin_id != entt::null) {
      if (auto pin_ptr = m_ecs.template try_get<pin_descriptor>(pin_id); pin_ptr && pin_ptr->type == type) {
        m_ecs.template emplace<opaque_link>(pin_id, create_link_delegate, destroy_link_delegate);
        node.pins.push_back(pin_id);
        tag_pin<TagT>(node, pin_id, index);
        if (type == pin_type::output && m_link_map[pin_id].size() > 0) {
          for (auto& to : m_link_map[pin_id]) {
            create_link_delegate(pin_id, to);
            // tag_pin<TagT>(node, to, index);
          }
        } else if (type == pin_type::input && m_link_map[pin_id].size() > 0) {
          for (auto& from : m_link_map[pin_id]) {
            create_link_delegate(from, pin_id);
            // tag_pin<TagT>(node, from, index);
          }
        }

        return *pin_ptr;
      }
    }

    auto pin_id = m_ecs.create();

    if (type != pin_type::internal) {
      m_ecs.template emplace<opaque_link>(pin_id, create_link_delegate, destroy_link_delegate);
    }

    tag_pin<TagT>(node, pin_id, index);
    node.pins.push_back(pin_id);
    return m_ecs.template emplace<pin_descriptor>(pin_id, pin_id, node.id, index, label, type);
  }

private:
  EcsT& m_ecs;
  // entt::dense_map<link_id, entt::dense_set<node_id>>  m_link_map;
  // entt::dense_map<pin_id, entt::dense_set<pin_id>>               m_link_map;
  std::unordered_map<entity, std::unordered_set<entity>> m_link_map;
  entt::dense_map<std::pair<pin_id, pin_id>, link_id>    m_link_index;
  entt::dense_map<pin_id, pin_id>                        m_io_map;
};
} // namespace entt_ext

CEREAL_CLASS_VERSION(entt_ext::node_descriptor, 2);