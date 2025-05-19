// Copyright 2019 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_UTIL_ZONE_H_
#define V8_UTIL_ZONE_H_

#include <list>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>

#include "ds/LifoAlloc.h"
#include "ds/Sort.h"
#include "irregexp/util/VectorShim.h"

namespace v8 {
namespace internal {

// V8::Zone ~= LifoAlloc
class Zone {
 public:
  Zone(js::LifoAlloc& alloc) : lifoAlloc_(alloc) {}

  template <typename T, typename... Args>
  T* New(Args&&... args) {
    js::LifoAlloc::AutoFallibleScope fallible(&lifoAlloc_);
    js::AutoEnterOOMUnsafeRegion oomUnsafe;
    void* memory = lifoAlloc_.alloc(sizeof(T));
    if (!memory) {
      oomUnsafe.crash("Irregexp Zone::New");
    }
    return new (memory) T(std::forward<Args>(args)...);
  }

  // Allocates uninitialized memory for 'length' number of T instances.
  template <typename T>
  T* NewArray(size_t length) {
    js::LifoAlloc::AutoFallibleScope fallible(&lifoAlloc_);
    js::AutoEnterOOMUnsafeRegion oomUnsafe;
    void* memory = lifoAlloc_.alloc(length * sizeof(T));
    if (!memory) {
      oomUnsafe.crash("Irregexp Zone::New");
    }
    return static_cast<T*>(memory);
  }

  void DeleteAll() { lifoAlloc_.freeAll(); }

  // Returns true if the total memory allocated exceeds a threshold.
  static const size_t kExcessLimit = 256 * 1024 * 1024;
  bool excess_allocation() const {
    return lifoAlloc_.computedSizeOfExcludingThis() > kExcessLimit;
  }

  js::LifoAlloc& inner() { return lifoAlloc_; }

 private:
  js::LifoAlloc& lifoAlloc_;
};

// Superclass for classes allocated in a Zone.
// Based on: https://github.com/v8/v8/blob/master/src/zone/zone.h
class ZoneObject {
 public:
  // new (zone) SomeObject(...) was the old pattern.
  // Delete the constructor to avoid using it accidentally.
  void* operator new(size_t size, Zone* zone) = delete;

  // Allow non-allocating placement new
  void* operator new(size_t size, void* ptr) { return ptr; }

  // Ideally, the delete operator should be private instead of
  // public, but unfortunately the compiler sometimes synthesizes
  // (unused) destructors for classes derived from ZoneObject, which
  // require the operator to be visible. MSVC requires the delete
  // operator to be public.

  // ZoneObjects should never be deleted individually; use
  // Zone::DeleteAll() to delete all zone objects in one go.
  void operator delete(void*, size_t) { MOZ_CRASH("unreachable"); }
  void operator delete(void* pointer, Zone* zone) { MOZ_CRASH("unreachable"); }
};

// ZoneLists are growable lists with constant-time access to the
// elements. The list itself and all its elements are allocated in the
// Zone. ZoneLists cannot be deleted individually; you can delete all
// objects in the Zone by calling Zone::DeleteAll().
// Used throughout irregexp.
// Based on: https://github.com/v8/v8/blob/master/src/zone/zone-list.h
template <typename T>
class ZoneList final : public ZoneObject {
 public:
  // Construct a new ZoneList with the given capacity; the length is
  // always zero. The capacity must be non-negative.
  ZoneList(int capacity, Zone* zone) : capacity_(capacity) {
    data_ = (capacity_ > 0) ? zone->NewArray<T>(capacity_) : nullptr;
  }
  // Construct a new ZoneList by copying the elements of the given ZoneList.
  ZoneList(const ZoneList<T>& other, Zone* zone)
      : ZoneList(other.length(), zone) {
    AddAll(other, zone);
  }

  // Construct a new ZoneList by copying the elements of the given vector.
  ZoneList(const base::Vector<const T>& other, Zone* zone)
      : ZoneList(other.length(), zone) {
    AddAll(other, zone);
  }

  ZoneList(ZoneList<T>&& other) { *this = std::move(other); }

  ZoneList& operator=(ZoneList&& other) {
    MOZ_ASSERT(!data_);
    data_ = other.data_;
    capacity_ = other.capacity_;
    length_ = other.length_;
    other.Clear();
    return *this;
  }

  // Returns a reference to the element at index i. This reference is not safe
  // to use after operations that can change the list's backing store
  // (e.g. Add).
  inline T& operator[](int i) const {
    MOZ_ASSERT(i >= 0);
    MOZ_ASSERT(static_cast<unsigned>(i) < static_cast<unsigned>(length_));
    return data_[i];
  }
  inline T& at(int i) const { return operator[](i); }
  inline T& last() const { return at(length_ - 1); }
  inline T& first() const { return at(0); }

  using iterator = T*;
  inline iterator begin() const { return &data_[0]; }
  inline iterator end() const { return &data_[length_]; }

  inline bool is_empty() const { return length_ == 0; }
  inline int length() const { return length_; }
  inline int capacity() const { return capacity_; }

  base::Vector<T> ToVector() const { return base::Vector<T>(data_, length_); }
  base::Vector<T> ToVector(int start, int length) const {
    return base::Vector<T>(data_ + start, std::min(length_ - start, length));
  }

  base::Vector<const T> ToConstVector() const {
    return base::Vector<const T>(data_, length_);
  }

  // Adds a copy of the given 'element' to the end of the list,
  // expanding the list if necessary.
  void Add(const T& element, Zone* zone) {
    if (length_ < capacity_) {
      data_[length_++] = element;
    } else {
      ZoneList<T>::ResizeAdd(element, zone);
    }
  }
  // Add all the elements from the argument list to this list.
  void AddAll(const ZoneList<T>& other, Zone* zone) {
    AddAll(other.ToVector(), zone);
  }
  // Add all the elements from the vector to this list.
  void AddAll(const base::Vector<const T>& other, Zone* zone) {
    int result_length = length_ + other.length();
    if (capacity_ < result_length) {
      Resize(result_length, zone);
    }
    if (std::is_fundamental<T>()) {
      memcpy(data_ + length_, other.begin(), sizeof(*data_) * other.length());
    } else {
      for (int i = 0; i < other.length(); i++) {
        data_[length_ + i] = other.at(i);
      }
    }
    length_ = result_length;
  }

  // Overwrites the element at the specific index.
  void Set(int index, const T& element) {
    MOZ_ASSERT(index >= 0 && index <= length_);
    data_[index] = element;
  }

  // Removes the i'th element without deleting it even if T is a
  // pointer type; moves all elements above i "down". Returns the
  // removed element.  This function's complexity is linear in the
  // size of the list.
  T Remove(int i) {
    T element = at(i);
    length_--;
    while (i < length_) {
      data_[i] = data_[i + 1];
      i++;
    }
    return element;
  }

  // Removes the last element without deleting it even if T is a
  // pointer type. Returns the removed element.
  inline T RemoveLast() { return Remove(length_ - 1); }

  // Clears the list, setting the capacity and length to 0.
  inline void Clear() {
    data_ = nullptr;
    capacity_ = 0;
    length_ = 0;
  }

  // Drops all but the first 'pos' elements from the list.
  inline void Rewind(int pos) {
    MOZ_ASSERT(0 <= pos && pos <= length_);
    length_ = pos;
  }

  inline bool Contains(const T& elm) const {
    for (int i = 0; i < length_; i++) {
      if (data_[i] == elm) return true;
    }
    return false;
  }

  template <typename CompareFunction>
  void StableSort(CompareFunction cmp, size_t start, size_t length) {
    js::AutoEnterOOMUnsafeRegion oomUnsafe;
    T* scratch = static_cast<T*>(js_malloc(length * sizeof(T)));
    if (!scratch) {
      oomUnsafe.crash("Irregexp stable sort scratch space");
    }
    auto comparator = [cmp](const T& a, const T& b, bool* lessOrEqual) {
      *lessOrEqual = cmp(&a, &b) <= 0;
      return true;
    };
    MOZ_ALWAYS_TRUE(
        js::MergeSort(begin() + start, length, scratch, comparator));
    js_free(scratch);
  }

  void operator delete(void* pointer) { MOZ_CRASH("unreachable"); }
  void operator delete(void* pointer, Zone* zone) { MOZ_CRASH("unreachable"); }

 private:
  T* data_ = nullptr;
  int capacity_ = 0;
  int length_ = 0;

  // Increase the capacity of a full list, and add an element.
  // List must be full already.
  void ResizeAdd(const T& element, Zone* zone) {
    MOZ_ASSERT(length_ >= capacity_);
    // Grow the list capacity by 100%, but make sure to let it grow
    // even when the capacity is zero (possible initial case).
    int new_capacity = 1 + 2 * capacity_;
    // Since the element reference could be an element of the list, copy
    // it out of the old backing storage before resizing.
    T temp = element;
    Resize(new_capacity, zone);
    data_[length_++] = temp;
  }

  // Resize the list.
  void Resize(int new_capacity, Zone* zone) {
    MOZ_ASSERT(length_ <= new_capacity);
    static_assert(std::is_trivially_copyable<T>::value);
    T* new_data = zone->NewArray<T>(new_capacity);
    if (length_ > 0) {
      memcpy(new_data, data_, length_ * sizeof(T));
    }
    data_ = new_data;
    capacity_ = new_capacity;
  }

  ZoneList& operator=(const ZoneList&) = delete;
  ZoneList() = delete;
  ZoneList(const ZoneList&) = delete;
};

// Based on: https://github.com/v8/v8/blob/master/src/zone/zone-allocator.h
template <typename T>
class ZoneAllocator {
 public:
  using pointer = T*;
  using const_pointer = const T*;
  using reference = T&;
  using const_reference = const T&;
  using value_type = T;
  using size_type = size_t;
  using difference_type = ptrdiff_t;
  template <class O>
  struct rebind {
    using other = ZoneAllocator<O>;
  };

  explicit ZoneAllocator(Zone* zone) : zone_(zone) {}
  template <typename U>
  ZoneAllocator(const ZoneAllocator<U>& other)
      : ZoneAllocator<T>(other.zone_) {}
  template <typename U>
  friend class ZoneAllocator;

  T* allocate(size_t n) { return zone_->NewArray<T>(n); }
  void deallocate(T* p, size_t) {}  // noop for zones

  bool operator==(ZoneAllocator const& other) const {
    return zone_ == other.zone_;
  }
  bool operator!=(ZoneAllocator const& other) const {
    return zone_ != other.zone_;
  }

  using Policy = js::LifoAllocPolicy<js::Fallible>;
  Policy policy() const {
    return js::LifoAllocPolicy<js::Fallible>(zone_->inner());
  }

 private:
  Zone* zone_;
};

// Zone wrappers for std containers:
// Origin:
// https://github.com/v8/v8/blob/5e514a969376dc63517d575b062758efd36cd757/src/zone/zone-containers.h#L25-L169

// A wrapper subclass for std::vector to make it easy to construct one
// that uses a zone allocator.
// Used throughout irregexp
template <typename T>
class ZoneVector : public std::vector<T, ZoneAllocator<T>> {
 public:
  ZoneVector(Zone* zone)
      : std::vector<T, ZoneAllocator<T>>(ZoneAllocator<T>(zone)) {}

  // Constructs a new vector and fills it with {size} elements, each
  // constructed via the default constructor.
  ZoneVector(size_t size, Zone* zone)
      : std::vector<T, ZoneAllocator<T>>(size, T(), ZoneAllocator<T>(zone)) {}

  // Constructs a new vector and fills it with the contents of the range
  // [first, last).
  template <class Iter>
  ZoneVector(Iter first, Iter last, Zone* zone)
      : std::vector<T, ZoneAllocator<T>>(first, last, ZoneAllocator<T>(zone)) {}
};

// A wrapper subclass for std::list to make it easy to construct one
// that uses a zone allocator.
// Used in regexp-bytecode-peephole.cc
template <typename T>
class ZoneLinkedList : public std::list<T, ZoneAllocator<T>> {
 public:
  // Constructs an empty list.
  explicit ZoneLinkedList(Zone* zone)
      : std::list<T, ZoneAllocator<T>>(ZoneAllocator<T>(zone)) {}
};

// A wrapper subclass for std::set to make it easy to construct one that uses
// a zone allocator.
// Used in regexp-parser.cc
template <typename K, typename Compare = std::less<K>>
class ZoneSet : public std::set<K, Compare, ZoneAllocator<K>> {
 public:
  // Constructs an empty set.
  explicit ZoneSet(Zone* zone)
      : std::set<K, Compare, ZoneAllocator<K>>(Compare(),
                                               ZoneAllocator<K>(zone)) {}
};

// A wrapper subclass for std::map to make it easy to construct one that uses
// a zone allocator.
// Used in regexp-bytecode-peephole.cc
template <typename K, typename V, typename Compare = std::less<K>>
class ZoneMap
    : public std::map<K, V, Compare, ZoneAllocator<std::pair<const K, V>>> {
 public:
  // Constructs an empty map.
  explicit ZoneMap(Zone* zone)
      : std::map<K, V, Compare, ZoneAllocator<std::pair<const K, V>>>(
            Compare(), ZoneAllocator<std::pair<const K, V>>(zone)) {}
};

// A wrapper subclass for std::unordered_map to make it easy to construct one
// that uses a zone allocator.
// Used in regexp-bytecode-peephole.cc
template <typename K, typename V, typename Hash = std::hash<K>,
          typename KeyEqual = std::equal_to<K>>
class ZoneUnorderedMap
    : public std::unordered_map<K, V, Hash, KeyEqual,
                                ZoneAllocator<std::pair<const K, V>>> {
 public:
  // Constructs an empty map.
  explicit ZoneUnorderedMap(Zone* zone, size_t bucket_count = 100)
      : std::unordered_map<K, V, Hash, KeyEqual,
                           ZoneAllocator<std::pair<const K, V>>>(
            bucket_count, Hash(), KeyEqual(),
            ZoneAllocator<std::pair<const K, V>>(zone)) {}
};

// A wrapper subclass for base::SmallVector to make it easy to construct one
// that uses a zone allocator.
template <typename T, size_t kSize>
class SmallZoneVector : public base::SmallVector<T, kSize, ZoneAllocator<T>> {
 public:
  // Constructs an empty small vector.
  explicit SmallZoneVector(Zone* zone)
      : base::SmallVector<T, kSize, ZoneAllocator<T>>(ZoneAllocator<T>(zone)) {}

  explicit SmallZoneVector(size_t size, Zone* zone)
      : base::SmallVector<T, kSize, ZoneAllocator<T>>(
            size, ZoneAllocator<T>(ZoneAllocator<T>(zone))) {}
};

}  // namespace internal
}  // namespace v8

#endif  // V8_UTIL_FLAG_H_
