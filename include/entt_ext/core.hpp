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
  std::atomic<size_t> m_index{0}; // index of the READ buffer

public:
  double_buffer() = default;
  double_buffer(const double_buffer& other)
    : m_buffer(other.m_buffer)
    , m_index(other.m_index.load(std::memory_order_relaxed)) {}

  double_buffer(double_buffer&& other)
    : m_buffer(std::move(other.m_buffer))
    , m_index(other.m_index.load(std::memory_order_relaxed)) {}

  double_buffer& operator=(const double_buffer& other) {
    if (this != &other) {
      m_buffer = other.m_buffer;
      m_index.store(other.m_index.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }
    return *this;
  }

  double_buffer& operator=(double_buffer&& other) {
    if (this != &other) {
      m_buffer = std::move(other.m_buffer);
      m_index.store(other.m_index.load(std::memory_order_relaxed), std::memory_order_relaxed);
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

  operator T&() { return write(); }
  operator const T&() const { return read(); }

  const T& read() const { return m_buffer[m_index.load(std::memory_order_acquire)]; }
  T&       write()       { return m_buffer[1 - m_index.load(std::memory_order_relaxed)]; }

  void swap() { m_index.store(1 - m_index.load(std::memory_order_relaxed), std::memory_order_release); }

  T*       operator->()       { return &write(); }
  const T* operator->() const { return &read(); }
  const T& operator*()  const { return read(); }
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