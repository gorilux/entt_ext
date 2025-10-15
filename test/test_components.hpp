#pragma once

#include <cereal/archives/portable_binary.hpp>
#include <cereal/types/string.hpp>

// Test components for sync testing
struct Position {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;

  bool operator==(const Position& other) const {
    return x == other.x && y == other.y && z == other.z;
  }

  template <typename Archive>
  void serialize(Archive& archive) {
    archive(x, y, z);
  }
};

struct Velocity {
  float dx = 0.0f;
  float dy = 0.0f;
  float dz = 0.0f;

  bool operator==(const Velocity& other) const {
    return dx == other.dx && dy == other.dy && dz == other.dz;
  }

  template <typename Archive>
  void serialize(Archive& archive) {
    archive(dx, dy, dz);
  }
};

struct Health {
  int current = 100;
  int maximum = 100;

  bool operator==(const Health& other) const {
    return current == other.current && maximum == other.maximum;
  }

  template <typename Archive>
  void serialize(Archive& archive) {
    archive(current, maximum);
  }
};

struct Name {
  std::string value;

  bool operator==(const Name& other) const {
    return value == other.value;
  }

  template <typename Archive>
  void serialize(Archive& archive) {
    archive(value);
  }
};

// Specializations for type_name
namespace entt_ext {
template <>
constexpr auto type_name<Position>() {
  return std::string_view{"Position"};
}
template <>
constexpr auto type_name<Velocity>() {
  return std::string_view{"Velocity"};
}
template <>
constexpr auto type_name<Health>() {
  return std::string_view{"Health"};
}
template <>
constexpr auto type_name<Name>() {
  return std::string_view{"Name"};
}
} // namespace entt_ext
