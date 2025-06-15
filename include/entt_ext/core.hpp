#pragma once

#ifdef ENTT_ID_TYPE
#undef ENTT_ID_TYPE
#endif

#include <cstdint>
#define ENTT_ID_TYPE std::uint64_t

#include <entt/entity/entity.hpp>

#include <boost/asio.hpp>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>

namespace entt_ext {
namespace asio        = boost::asio;
namespace asio_detail = asio::experimental::awaitable_operators::detail;

using entity  = entt::entity;
using null_t  = entt::null_t;
using id_type = entt::id_type;

inline constexpr null_t null{};
struct delete_later {
  // Empty struct needs explicit definition
  delete_later() = default;
};

template <typename T>
class double_buffer {
  std::array<T, 2>    m_buffer{};
  std::atomic<size_t> m_read_index{0};
  std::atomic<size_t> m_write_index{1};

public:
  double_buffer() = default;
  double_buffer(const double_buffer& other) {
    if (this != &other) {
      m_buffer      = other.m_buffer;
      m_read_index  = other.m_read_index.load();
      m_write_index = other.m_write_index.load();
    }
  }

  double_buffer(double_buffer&& other) {
    if (this != &other) {
      m_buffer      = std::move(other.m_buffer);
      m_read_index  = other.m_read_index.load();
      m_write_index = other.m_write_index.load();
    }
  }

  double_buffer& operator=(const double_buffer& other) {
    if (this != &other) {
      m_buffer      = other.m_buffer;
      m_read_index  = other.m_read_index.load();
      m_write_index = other.m_write_index.load();
    }
    return *this;
  }

  double_buffer& operator=(double_buffer&& other) {
    if (this != &other) {
      m_buffer      = std::move(other.m_buffer);
      m_read_index  = other.m_read_index.load();
      m_write_index = other.m_write_index.load();
    }
    return *this;
  }

  double_buffer& operator=(T&& other) {
    write() = std::move(other);
    swap();
    return *this;
  }

  double_buffer& operator=(T const& other) {
    write() = other;
    swap();
    return *this;
  }

  operator T&() {
    return write();
  }

  operator const T&() const {
    return read();
  }

  const T& read() const {
    // if constexpr (std::is_same_v<T, bool>) {
    //   spdlog::debug("[read] read index {} {} read_value {} write_value {} ",
    //                 m_read_index.load(),
    //                 m_write_index.load(),
    //                 m_buffer[m_read_index],
    //                 m_buffer[m_write_index]);
    // }
    return m_buffer[m_read_index];
  }

  T& write() {
    // if constexpr (std::is_same_v<T, bool>) {
    //   spdlog::debug("[write] read index {} {} read_value {} write_value {} ",
    //                 m_read_index.load(),
    //                 m_write_index.load(),
    //                 m_buffer[m_read_index],
    //                 m_buffer[m_write_index]);
    // }
    return m_buffer[m_write_index];
  }

  void swap() {
    // if constexpr (std::is_same_v<T, bool>) {
    //   spdlog::debug("[swap] read index {} {} read_value {} write_value {} ",
    //                 m_read_index.load(),
    //                 m_write_index.load(),
    //                 m_buffer[m_read_index],
    //                 m_buffer[m_write_index]);
    // }
    m_read_index.store(m_write_index.load());
    m_write_index.store((m_read_index.load() + 1) % 2);
  }

  T* operator->() {
    return &write();
  }

  const T* operator->() const {
    return &read();
  }

  const T& operator*() const {
    return read();
  }
};

} // namespace entt_ext

namespace std {
template <>
struct hash<entt_ext::entity> {
  inline auto operator()(const entt_ext::entity& e) const -> size_t {
    return std::hash<size_t>{}(static_cast<size_t>(e));
  }
};
template <>
struct hash<std::pair<entt_ext::entity, entt_ext::entity>> {
  inline auto operator()(const std::pair<entt_ext::entity, entt_ext::entity>& kvp) const -> size_t {
    return std::hash<entt_ext::entity>{}(kvp.first) ^ std::hash<entt_ext::entity>{}(kvp.second);
  }
};
template <>
struct hash<std::pair<entt_ext::entity, entt_ext::id_type>> {
  inline auto operator()(const std::pair<entt_ext::entity, entt_ext::id_type>& kvp) const -> size_t {
    return std::hash<entt_ext::entity>{}(kvp.first) ^ kvp.second;
  }
};
template <>
struct hash<std::pair<entt_ext::id_type, size_t>> {
  inline auto operator()(const std::pair<entt_ext::id_type, size_t>& kvp) const -> size_t {
    return kvp.first ^ kvp.second;
  }
};
} // namespace std