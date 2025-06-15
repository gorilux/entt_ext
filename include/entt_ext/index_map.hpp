#pragma once

#include <iterator>
#include <unordered_map>
#include <vector>

namespace entt_ext {

template <typename K, typename V, typename Tag = void>
class index_map {
public:
  // Nested iterator class
  class iterator {
  public:
    using iterator_category = std::forward_iterator_tag;
    using value_type        = typename std::vector<std::pair<K, V>>::value_type;
    using difference_type   = std::ptrdiff_t;
    using pointer           = value_type*;
    using reference         = value_type&;

    iterator(typename std::vector<std::pair<K, V>>::iterator it)
      : it(it) {
    }

    reference operator*() {
      return *it;
    }

    pointer operator->() {
      return &(*it);
    }

    iterator& operator++() {
      ++it;
      return *this;
    }

    iterator operator++(int) {
      iterator temp = *this;
      ++(*this);
      return temp;
    }

    bool operator==(const iterator& other) const {
      return it == other.it;
    }

    bool operator!=(const iterator& other) const {
      return it != other.it;
    }

  private:
    typename std::vector<std::pair<K, V>>::iterator it;
  };

  // Nested const_iterator class
  class const_iterator {
  public:
    using iterator_category = std::forward_iterator_tag;
    using value_type        = const typename std::vector<std::pair<K, V>>::value_type;
    using difference_type   = std::ptrdiff_t;
    using pointer           = value_type*;
    using reference         = value_type&;

    const_iterator(typename std::vector<std::pair<K, V>>::const_iterator it)
      : it(it) {
    }

    reference operator*() const {
      return *it;
    }

    pointer operator->() const {
      return &(*it);
    }

    const_iterator& operator++() {
      ++it;
      return *this;
    }

    const_iterator operator++(int) {
      const_iterator temp = *this;
      ++(*this);
      return temp;
    }

    bool operator==(const const_iterator& other) const {
      return it == other.it;
    }

    bool operator!=(const const_iterator& other) const {
      return it != other.it;
    }

  private:
    typename std::vector<std::pair<K, V>>::const_iterator it;
  };

  // Access element by key, inserting a default if the key is not present
  V& operator[](const K& key) {
    auto it = index_map.find(key);
    if (it != index_map.end()) {
      return elements[it->second].second;
    }
    size_t index   = elements.size();
    index_map[key] = index;
    elements.emplace_back(key, V{});
    return elements.back().second;
  }

  // Access element by key (const version)
  const V& operator[](const K& key) const {
    return elements.at(index_map.at(key)).second;
  }

  // Find an element by key and return an iterator
  iterator find(const K& key) {
    auto it = index_map.find(key);
    if (it != index_map.end()) {
      return iterator(elements.begin() + it->second);
    }
    return end();
  }

  // Find an element by key and return a const_iterator
  const_iterator find(const K& key) const {
    auto it = index_map.find(key);
    if (it != index_map.end()) {
      return const_iterator(elements.begin() + it->second);
    }
    return end();
  }

  // Remove an element by key while maintaining order
  bool erase(const K& key) {
    auto it = index_map.find(key);
    if (it == index_map.end()) {
      return false; // Key not found
    }

    size_t index = it->second;
    index_map.erase(it);

    // If it's not the last element, swap it with the last to keep order
    if (index != elements.size() - 1) {
      std::swap(elements[index], elements.back());
      index_map[elements[index].first] = index; // Update index of swapped element
    }

    elements.pop_back(); // Remove the last element (which is now redundant)
    return true;
  }

  // Clear all elements
  void clear() {
    elements.clear();
    index_map.clear();
  }

  // Iterators for key-value pairs
  iterator begin() {
    return iterator(elements.begin());
  }

  iterator end() {
    return iterator(elements.end());
  }

  const_iterator begin() const {
    return const_iterator(elements.begin());
  }

  const_iterator end() const {
    return const_iterator(elements.end());
  }

  const_iterator cbegin() const {
    return const_iterator(elements.begin());
  }

  const_iterator cend() const {
    return const_iterator(elements.end());
  }

  // Insert a new element by key and value
  V& emplace(const K& key, V&& value) {
    auto it = index_map.find(key);
    if (it != index_map.end()) {
      return elements[it->second].second;
    }
    size_t index   = elements.size();
    index_map[key] = index;
    elements.emplace_back(key, std::move(value));
    return elements.back().second;
  }

  // Check if the container is empty
  bool empty() const {
    return elements.empty();
  }

  // Remove the last element (by insertion order)
  void pop_back() {
    if (elements.empty()) {
      return;
    }
    K last_key = elements.back().first;
    index_map.erase(last_key);
    elements.pop_back();
  }

  // Get the size of the container
  size_t size() const {
    return elements.size();
  }

private:
  std::vector<std::pair<K, V>>  elements;  // Stores keys and values in insertion order
  std::unordered_map<K, size_t> index_map; // Maps keys to indices in the vector
};
} // namespace entt_ext