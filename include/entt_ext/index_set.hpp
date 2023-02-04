#pragma once

#include <algorithm>
#include <iterator>
#include <unordered_map>
#include <vector>

namespace entt_ext {

template <typename K, typename... Tag>
class index_set {
public:
  // Nested iterator class
  class iterator {
  public:
    using iterator_category = std::forward_iterator_tag;
    using value_type        = K;
    using difference_type   = std::ptrdiff_t;
    using pointer           = K*;
    using reference         = K&;

    iterator(typename std::vector<K>::iterator it)
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

    bool operator>(const iterator& other) const {
      return it > other.it;
    }

    bool operator<(const iterator& other) const {
      return it < other.it;
    }

    bool operator>=(const iterator& other) const {
      return it >= other.it;
    }

    bool operator<=(const iterator& other) const {
      return it <= other.it;
    }

    bool operator==(const iterator& other) const {
      return it == other.it;
    }

    bool operator!=(const iterator& other) const {
      return it != other.it;
    }

  private:
    typename std::vector<K>::iterator it;
  };

  // Nested const_iterator class
  class const_iterator {
  public:
    using iterator_category = std::forward_iterator_tag;
    using value_type        = const K;
    using difference_type   = std::ptrdiff_t;
    using pointer           = const K*;
    using reference         = const K&;

    const_iterator(typename std::vector<K>::const_iterator it)
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

    bool operator>(const const_iterator& other) const {
      return it > other.it;
    }

    bool operator<(const const_iterator& other) const {
      return it < other.it;
    }

    bool operator>=(const const_iterator& other) const {
      return it >= other.it;
    }

    bool operator<=(const const_iterator& other) const {
      return it <= other.it;
    }

    bool operator==(const const_iterator& other) const {
      return it == other.it;
    }

    bool operator!=(const const_iterator& other) const {
      return it != other.it;
    }

  private:
    typename std::vector<K>::const_iterator it;
  };

  // Constructors
  index_set()  = default;
  ~index_set() = default;

  // Insert a key into the set at the end.
  // Returns true if the key was inserted; false if the key already exists.
  bool insert(const K& key) {
    if (index_map.find(key) != index_map.end()) {
      return false; // Key already exists.
    }
    size_t index   = elements.size();
    index_map[key] = index;
    elements.push_back(key);
    return true;
  }

  // Insert a key into the set at a specified position.
  // Returns true if the key was inserted; false if the key already exists.
  bool insert_at(const K& key, size_t pos) {
    if (index_map.find(key) != index_map.end()) {
      return false; // Key already exists.
    }
    // Clamp pos to be within the valid range.
    if (pos > elements.size()) {
      pos = elements.size();
    }
    // Insert key at the specified position.
    elements.insert(elements.begin() + pos, key);
    // Update indices for the inserted key and all keys after it.
    for (size_t i = pos; i < elements.size(); ++i) {
      index_map[elements[i]] = i;
    }
    return true;
  }

  // Insert a key before a given position.
  bool insert_before(size_t pos, const K& key) {
    return insert_at(key, pos);
  }

  // Insert a key before the given reference key.
  bool insert_before(const K& ref_key, const K& key) {
    auto it = index_map.find(ref_key);
    if (it == index_map.end()) {
      return false; // Reference key not found.
    }
    size_t pos = it->second;
    return insert_at(key, pos);
  }

  // Insert a key after a given position.
  bool insert_after(size_t pos, const K& key) {
    if (pos >= elements.size()) {
      pos = elements.size();
    } else {
      pos = pos + 1;
    }
    return insert_at(key, pos);
  }

  // Insert a key after the given reference key.
  bool insert_after(const K& ref_key, const K& key) {
    auto it = index_map.find(ref_key);
    if (it == index_map.end()) {
      return false; // Reference key not found.
    }
    size_t pos = it->second + 1;
    return insert_at(key, pos);
  }

  // Swap the elements at two specified positions.
  // Returns true if swap was successful; false if either position is invalid.
  bool swap(size_t pos1, size_t pos2) {
    if (pos1 >= elements.size() || pos2 >= elements.size()) {
      return false; // Invalid positions.
    }
    if (pos1 == pos2) {
      return true; // Nothing to do.
    }
    std::swap(elements[pos1], elements[pos2]);
    // Update the indices in the mapping for both swapped keys.
    index_map[elements[pos1]] = pos1;
    index_map[elements[pos2]] = pos2;
    return true;
  }

  // Move an item from one position to another, preserving order.
  // Returns true if move was successful; false if indices are out of range.
  bool move_item(size_t from, size_t to) {
    if (from >= elements.size() || to >= elements.size()) {
      return false; // Out of range.
    }
    if (from == to) {
      return true; // Nothing to do.
    }
    if (from < to) {
      auto first  = elements.begin() + from;
      auto middle = first + 1;
      auto last   = elements.begin() + to + 1;
      std::rotate(first, middle, last);
    } else { // from > to
      auto first  = elements.begin() + to;
      auto middle = elements.begin() + from;
      auto last   = elements.begin() + from + 1;
      std::rotate(first, middle, last);
    }
    // Update mapping for affected range.
    size_t start = std::min(from, to);
    size_t end   = std::max(from, to);
    for (size_t i = start; i <= end; ++i) {
      index_map[elements[i]] = i;
    }
    return true;
  }

  // Move an item specified by key to a new position.
  bool move_item(const K& key, size_t to) {
    auto it = index_map.find(key);
    if (it == index_map.end()) {
      return false; // Key not found.
    }
    size_t from = it->second;
    return move_item(from, to);
  }

  // Erase a key from the set while maintaining the order.
  // Returns true if the key was found and removed; false otherwise.
  bool erase(const K& key) {
    auto it = index_map.find(key);
    if (it == index_map.end()) {
      return false; // Key not found.
    }
    size_t pos = it->second;
    // Erase the key from the mapping.
    index_map.erase(it);
    // Remove the element from the vector (shifts subsequent elements left).
    elements.erase(elements.begin() + pos);
    // Update the indices for all elements that followed the removed element.
    for (size_t i = pos; i < elements.size(); ++i) {
      index_map[elements[i]] = i;
    }
    return true;
  }

  // Remove the key at a specific position while preserving order.
  // Returns true if removal was successful; false if the position is invalid.
  bool remove_at(size_t pos) {
    if (pos >= elements.size()) {
      return false; // Invalid position.
    }
    // Remove the key from the index_map.
    K key = elements[pos];
    index_map.erase(key);
    // Erase the element from the vector.
    elements.erase(elements.begin() + pos);
    // Update the indices for all elements after the removed position.
    for (size_t i = pos; i < elements.size(); ++i) {
      index_map[elements[i]] = i;
    }
    return true;
  }

  // Move an item so that it appears immediately after the specified reference key.
  bool move_item_after(const K& key_item, const K& key_after) {
    auto it_after = index_map.find(key_after);
    if (it_after == index_map.end()) {
      return false; // Reference key not found.
    }
    size_t new_index = it_after->second + 1;
    return move_item(key_item, new_index);
  }

  // Move an item so that it appears immediately before the specified reference key.
  bool move_item_before(const K& key_item, const K& key_before) {
    auto it_before = index_map.find(key_before);
    if (it_before == index_map.end()) {
      return false; // Reference key not found.
    }
    size_t new_index = it_before->second;
    return move_item(key_item, new_index);
  }

  // Smart move: Move key_item relative to key_before_or_after.
  // If key_item is currently before key_before_or_after, it will be moved to appear after.
  // Otherwise, it will be moved to appear before.
  bool move_item(const K& key_item, const K& key_before_or_after) {
    auto it_item = index_map.find(key_item);
    auto it_ref  = index_map.find(key_before_or_after);
    if (it_item == index_map.end() || it_ref == index_map.end()) {
      return false;
    }
    size_t pos_item = it_item->second;
    size_t pos_ref  = it_ref->second;
    if (pos_item == pos_ref) {
      // They are the same element; nothing to do.
      return true;
    } else if (pos_item < pos_ref) {
      // key_item is before key_before_or_after; move it to appear after.
      return move_item_after(key_item, key_before_or_after);
    } else {
      // key_item is after key_before_or_after; move it to appear before.
      return move_item_before(key_item, key_before_or_after);
    }
  }

  // Remove the last element (by insertion order).
  void pop_back() {
    if (elements.empty()) {
      return;
    }
    K last_key = elements.back();
    index_map.erase(last_key);
    elements.pop_back();
  }

  // Clear all elements.
  void clear() {
    elements.clear();
    index_map.clear();
  }

  // Container-level swap: Swap the entire contents with another index_set.
  void swap(index_set& other) noexcept {
    using std::swap;
    swap(elements, other.elements);
    swap(index_map, other.index_map);
  }

  // Iterators for the stored keys.
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

  // Check if the set contains a key.
  bool contains(const K& key) const {
    return index_map.find(key) != index_map.end();
  }

  // Find a key and return an iterator to it.
  iterator find(const K& key) {
    auto it = index_map.find(key);
    if (it != index_map.end()) {
      return iterator(elements.begin() + it->second);
    }
    return end();
  }

  // Find a key and return a const_iterator to it.
  const_iterator find(const K& key) const {
    auto it = index_map.find(key);
    if (it != index_map.end()) {
      return const_iterator(elements.begin() + it->second);
    }
    return end();
  }

  // Return the number of elements in the set.
  size_t size() const {
    return elements.size();
  }

  // Check if the set is empty.
  bool empty() const {
    return elements.empty();
  }

  // Sort the elements using a custom comparator.
  // After sorting, the index_map is updated to match the new order.
  template <typename Compare>
  void sort(Compare comp) {
    std::sort(elements.begin(), elements.end(), comp);
    // Update index_map to reflect new order
    for (size_t i = 0; i < elements.size(); ++i) {
      index_map[elements[i]] = i;
    }
  }

  // Sort the elements using operator<.
  void sort() {
    std::sort(elements.begin(), elements.end());
    // Update index_map to reflect new order
    for (size_t i = 0; i < elements.size(); ++i) {
      index_map[elements[i]] = i;
    }
  }

private:
  std::vector<K>                elements;  // Stores keys in insertion order.
  std::unordered_map<K, size_t> index_map; // Maps keys to their index in the vector.
};

// Non-member swap overload for index_set.
template <typename K>
void swap(index_set<K>& lhs, index_set<K>& rhs) noexcept {
  lhs.swap(rhs);
}

} // namespace entt_ext