/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef ds_InlineTable_h
#define ds_InlineTable_h

#include "mozilla/Maybe.h"

#include <utility>

#include "js/AllocPolicy.h"
#include "js/HashTable.h"

namespace js {

namespace detail {

// The InlineTable below needs an abstract way of testing keys for
// tombstone values, and to set a key in an entry to a tombstone.
// This is provided by the KeyPolicy generic type argument, which
// has a default implementation for pointers provided below.

// A default implementation of a KeyPolicy for some types (only pointer
// types for now).
//
// The `KeyPolicy` type parameter informs an InlineTable of how to
// check for tombstone values and to set tombstone values within
// the domain of key (entry).
//
// A `KeyPolicy` for some key type `K` must provide two static methods:
//   static bool isTombstone(const K& key);
//   static void setToTombstone(K& key);
template <typename K>
class DefaultKeyPolicy;

template <typename T>
class DefaultKeyPolicy<T*> {
  DefaultKeyPolicy() = delete;
  DefaultKeyPolicy(const T*&) = delete;

 public:
  static bool isTombstone(T* const& ptr) { return ptr == nullptr; }
  static void setToTombstone(T*& ptr) { ptr = nullptr; }
};

template <typename InlineEntry, typename Entry, typename Table,
          typename HashPolicy, typename AllocPolicy, typename KeyPolicy,
          size_t InlineEntries>
class InlineTable : private AllocPolicy {
 private:
  using TablePtr = typename Table::Ptr;
  using TableAddPtr = typename Table::AddPtr;
  using TableRange = typename Table::Range;
  using Lookup = typename HashPolicy::Lookup;

  size_t inlNext_;
  size_t inlCount_;
  InlineEntry inl_[InlineEntries];
  Table table_;

#ifdef DEBUG
  template <typename Key>
  static bool keyNonZero(const Key& key) {
    // Zero as tombstone means zero keys are invalid.
    return !!key;
  }
#endif

  InlineEntry* inlineStart() {
    MOZ_ASSERT(!usingTable());
    return inl_;
  }

  const InlineEntry* inlineStart() const {
    MOZ_ASSERT(!usingTable());
    return inl_;
  }

  InlineEntry* inlineEnd() {
    MOZ_ASSERT(!usingTable());
    return inl_ + inlNext_;
  }

  const InlineEntry* inlineEnd() const {
    MOZ_ASSERT(!usingTable());
    return inl_ + inlNext_;
  }

  bool usingTable() const { return inlNext_ > InlineEntries; }

  [[nodiscard]] bool switchToTable() {
    MOZ_ASSERT(inlNext_ == InlineEntries);

    table_.clearAndCompact();

    InlineEntry* end = inlineEnd();
    for (InlineEntry* it = inlineStart(); it != end; ++it) {
      if (it->key && !it->moveTo(table_)) {
        return false;
      }
    }

    inlNext_ = InlineEntries + 1;
    MOZ_ASSERT(table_.count() == inlCount_);
    MOZ_ASSERT(usingTable());
    return true;
  }

  [[nodiscard]] MOZ_NEVER_INLINE bool switchAndAdd(const InlineEntry& entry) {
    if (!switchToTable()) {
      return false;
    }

    return entry.putNew(table_);
  }

 public:
  static const size_t SizeOfInlineEntries = sizeof(InlineEntry) * InlineEntries;

  explicit InlineTable(AllocPolicy a = AllocPolicy())
      : AllocPolicy(std::move(a)), inlNext_(0), inlCount_(0), table_(a) {}

  class Ptr {
    friend class InlineTable;

   protected:
    MOZ_INIT_OUTSIDE_CTOR Entry entry_;
    MOZ_INIT_OUTSIDE_CTOR TablePtr tablePtr_;
    MOZ_INIT_OUTSIDE_CTOR InlineEntry* inlPtr_;
    MOZ_INIT_OUTSIDE_CTOR bool isInlinePtr_;

    explicit Ptr(TablePtr p)
        : entry_(p.found() ? &*p : nullptr),
          tablePtr_(p),
          isInlinePtr_(false) {}

    explicit Ptr(InlineEntry* inlineEntry)
        : entry_(inlineEntry), inlPtr_(inlineEntry), isInlinePtr_(true) {}

    void operator==(const Ptr& other);

   public:
    // Leaves Ptr uninitialized.
    Ptr() {
#ifdef DEBUG
      inlPtr_ = (InlineEntry*)0xbad;
      isInlinePtr_ = true;
#endif
    }

    // Default copy constructor works for this structure.

    bool found() const {
      return isInlinePtr_ ? bool(inlPtr_) : tablePtr_.found();
    }

    explicit operator bool() const { return found(); }

    bool operator==(const Ptr& other) const {
      MOZ_ASSERT(found() && other.found());
      if (isInlinePtr_ != other.isInlinePtr_) {
        return false;
      }
      if (isInlinePtr_) {
        return inlPtr_ == other.inlPtr_;
      }
      return tablePtr_ == other.tablePtr_;
    }

    bool operator!=(const Ptr& other) const { return !(*this == other); }

    Entry& operator*() {
      MOZ_ASSERT(found());
      return entry_;
    }

    Entry* operator->() {
      MOZ_ASSERT(found());
      return &entry_;
    }
  };

  class AddPtr {
    friend class InlineTable;

   protected:
    MOZ_INIT_OUTSIDE_CTOR Entry entry_;
    MOZ_INIT_OUTSIDE_CTOR TableAddPtr tableAddPtr_;
    MOZ_INIT_OUTSIDE_CTOR InlineEntry* inlAddPtr_;
    MOZ_INIT_OUTSIDE_CTOR bool isInlinePtr_;
    // Indicates whether inlAddPtr is a found result or an add pointer.
    MOZ_INIT_OUTSIDE_CTOR bool inlPtrFound_;

    AddPtr(InlineEntry* ptr, bool found)
        : entry_(ptr),
          inlAddPtr_(ptr),
          isInlinePtr_(true),
          inlPtrFound_(found) {}

    explicit AddPtr(const TableAddPtr& p)
        : entry_(p.found() ? &*p : nullptr),
          tableAddPtr_(p),
          isInlinePtr_(false) {}

   public:
    AddPtr() = default;

    bool found() const {
      return isInlinePtr_ ? inlPtrFound_ : tableAddPtr_.found();
    }

    explicit operator bool() const { return found(); }

    bool operator==(const AddPtr& other) const {
      MOZ_ASSERT(found() && other.found());
      if (isInlinePtr_ != other.isInlinePtr_) {
        return false;
      }
      if (isInlinePtr_) {
        return inlAddPtr_ == other.inlAddPtr_;
      }
      return tableAddPtr_ == other.tableAddPtr_;
    }

    bool operator!=(const AddPtr& other) const { return !(*this == other); }

    Entry& operator*() {
      MOZ_ASSERT(found());
      return entry_;
    }

    Entry* operator->() {
      MOZ_ASSERT(found());
      return &entry_;
    }
  };

  size_t count() const { return usingTable() ? table_.count() : inlCount_; }

  bool empty() const { return usingTable() ? table_.empty() : !inlCount_; }

  void clear() {
    inlNext_ = 0;
    inlCount_ = 0;
  }

  MOZ_ALWAYS_INLINE
  Ptr lookup(const Lookup& l) {
    MOZ_ASSERT(keyNonZero(l));

    if (usingTable()) {
      return Ptr(table_.lookup(l));
    }

    InlineEntry* end = inlineEnd();
    for (InlineEntry* it = inlineStart(); it != end; ++it) {
      if (it->key && HashPolicy::match(it->key, l)) {
        return Ptr(it);
      }
    }

    return Ptr(nullptr);
  }

  MOZ_ALWAYS_INLINE
  AddPtr lookupForAdd(const Lookup& l) {
    MOZ_ASSERT(keyNonZero(l));

    if (usingTable()) {
      return AddPtr(table_.lookupForAdd(l));
    }

    InlineEntry* end = inlineEnd();
    for (InlineEntry* it = inlineStart(); it != end; ++it) {
      if (it->key && HashPolicy::match(it->key, l)) {
        return AddPtr(it, true);
      }
    }

    // The add pointer that's returned here may indicate the limit entry of
    // the linear space, in which case the |add| operation will initialize
    // the table if necessary and add the entry there.
    return AddPtr(inlineEnd(), false);
  }

  template <typename KeyInput, typename... Args>
  [[nodiscard]] MOZ_ALWAYS_INLINE bool add(AddPtr& p, KeyInput&& key,
                                           Args&&... args) {
    MOZ_ASSERT(!p);
    MOZ_ASSERT(keyNonZero(key));

    if (p.isInlinePtr_) {
      InlineEntry* addPtr = p.inlAddPtr_;
      MOZ_ASSERT(addPtr == inlineEnd());

      // Switching to table mode before we add this pointer.
      if (addPtr == inlineStart() + InlineEntries) {
        if (!switchToTable()) {
          return false;
        }
        return table_.putNew(std::forward<KeyInput>(key),
                             std::forward<Args>(args)...);
      }

      MOZ_ASSERT(!p.found());
      MOZ_ASSERT(uintptr_t(inlineEnd()) == uintptr_t(p.inlAddPtr_));

      if (!this->checkSimulatedOOM()) {
        return false;
      }

      addPtr->update(std::forward<KeyInput>(key), std::forward<Args>(args)...);
      ++inlCount_;
      ++inlNext_;
      return true;
    }

    return table_.add(p.tableAddPtr_, std::forward<KeyInput>(key),
                      std::forward<Args>(args)...);
  }

  void remove(Ptr& p) {
    MOZ_ASSERT(p);
    if (p.isInlinePtr_) {
      MOZ_ASSERT(inlCount_ > 0);
      MOZ_ASSERT(!KeyPolicy::isTombstone(p.inlPtr_->key));
      KeyPolicy::setToTombstone(p.inlPtr_->key);
      --inlCount_;
      return;
    }
    MOZ_ASSERT(usingTable());
    table_.remove(p.tablePtr_);
  }

  void remove(const Lookup& l) {
    if (Ptr p = lookup(l)) {
      remove(p);
    }
  }

  class Range {
    friend class InlineTable;

   protected:
    mozilla::Maybe<TableRange> tableRange_;  // `Nothing` if `isInline_==true`
    InlineEntry* cur_;
    InlineEntry* end_;
    bool isInline_;

    explicit Range(TableRange r)
        : tableRange_(mozilla::Some(r)),
          cur_(nullptr),
          end_(nullptr),
          isInline_(false) {
      MOZ_ASSERT(!isInlineRange());
    }

    Range(const InlineEntry* begin, const InlineEntry* end)
        : tableRange_(mozilla::Nothing()),
          cur_(const_cast<InlineEntry*>(begin)),
          end_(const_cast<InlineEntry*>(end)),
          isInline_(true) {
      advancePastNulls(cur_);
      MOZ_ASSERT(isInlineRange());
    }

    bool assertInlineRangeInvariants() const {
      MOZ_ASSERT(uintptr_t(cur_) <= uintptr_t(end_));
      MOZ_ASSERT_IF(cur_ != end_, !KeyPolicy::isTombstone(cur_->key));
      return true;
    }

    bool isInlineRange() const {
      MOZ_ASSERT_IF(isInline_, assertInlineRangeInvariants());
      return isInline_;
    }

    void advancePastNulls(InlineEntry* begin) {
      InlineEntry* newCur = begin;
      while (newCur < end_ && KeyPolicy::isTombstone(newCur->key)) {
        ++newCur;
      }
      MOZ_ASSERT(uintptr_t(newCur) <= uintptr_t(end_));
      cur_ = newCur;
    }

    void bumpCurPtr() {
      MOZ_ASSERT(isInlineRange());
      advancePastNulls(cur_ + 1);
    }

   public:
    bool empty() const {
      return isInlineRange() ? cur_ == end_ : tableRange_->empty();
    }

    Entry front() {
      MOZ_ASSERT(!empty());
      if (isInlineRange()) {
        return Entry(cur_);
      }
      return Entry(&tableRange_->front());
    }

    void popFront() {
      MOZ_ASSERT(!empty());
      if (isInlineRange()) {
        bumpCurPtr();
      } else {
        tableRange_->popFront();
      }
    }
  };

  Range all() const {
    return usingTable() ? Range(table_.all())
                        : Range(inlineStart(), inlineEnd());
  }
};

}  // namespace detail

// A map with InlineEntries number of entries kept inline in an array.
//
// The Key type must be zeroable as zeros are used as tombstone keys.
// The Value type must have a default constructor.
//
// The API is very much like HashMap's.
template <typename Key, typename Value, size_t InlineEntries,
          typename HashPolicy = DefaultHasher<Key>,
          typename AllocPolicy = TempAllocPolicy,
          typename KeyPolicy = detail::DefaultKeyPolicy<Key>>
class InlineMap {
  using Map = HashMap<Key, Value, HashPolicy, AllocPolicy>;

  struct InlineEntry {
    Key key;
    Value value;

    template <typename KeyInput, typename ValueInput>
    void update(KeyInput&& key, ValueInput&& value) {
      this->key = std::forward<KeyInput>(key);
      this->value = std::forward<ValueInput>(value);
    }

    [[nodiscard]] bool moveTo(Map& map) {
      return map.putNew(std::move(key), std::move(value));
    }
  };

  class Entry {
    using MapEntry = typename Map::Entry;

    MapEntry* mapEntry_;
    InlineEntry* inlineEntry_;

   public:
    Entry() = default;

    explicit Entry(MapEntry* mapEntry)
        : mapEntry_(mapEntry), inlineEntry_(nullptr) {}

    explicit Entry(InlineEntry* inlineEntry)
        : mapEntry_(nullptr), inlineEntry_(inlineEntry) {}

    const Key& key() const {
      MOZ_ASSERT(!!mapEntry_ != !!inlineEntry_);
      if (mapEntry_) {
        return mapEntry_->key();
      }
      return inlineEntry_->key;
    }

    Value& value() {
      MOZ_ASSERT(!!mapEntry_ != !!inlineEntry_);
      if (mapEntry_) {
        return mapEntry_->value();
      }
      return inlineEntry_->value;
    }
  };

  using Impl = detail::InlineTable<InlineEntry, Entry, Map, HashPolicy,
                                   AllocPolicy, KeyPolicy, InlineEntries>;

  Impl impl_;

 public:
  using Table = Map;
  using Ptr = typename Impl::Ptr;
  using AddPtr = typename Impl::AddPtr;
  using Range = typename Impl::Range;
  using Lookup = typename HashPolicy::Lookup;

  static const size_t SizeOfInlineEntries = Impl::SizeOfInlineEntries;

  explicit InlineMap(AllocPolicy a = AllocPolicy()) : impl_(std::move(a)) {}

  size_t count() const { return impl_.count(); }

  bool empty() const { return impl_.empty(); }

  void clear() { impl_.clear(); }

  Range all() const { return impl_.all(); }

  MOZ_ALWAYS_INLINE
  Ptr lookup(const Lookup& l) { return impl_.lookup(l); }

  MOZ_ALWAYS_INLINE
  bool has(const Lookup& l) const {
    return const_cast<InlineMap*>(this)->lookup(l).found();
  }

  MOZ_ALWAYS_INLINE
  AddPtr lookupForAdd(const Lookup& l) { return impl_.lookupForAdd(l); }

  template <typename KeyInput, typename ValueInput>
  [[nodiscard]] MOZ_ALWAYS_INLINE bool add(AddPtr& p, KeyInput&& key,
                                           ValueInput&& value) {
    return impl_.add(p, std::forward<KeyInput>(key),
                     std::forward<ValueInput>(value));
  }

  template <typename KeyInput, typename ValueInput>
  [[nodiscard]] bool put(KeyInput&& key, ValueInput&& value) {
    AddPtr p = lookupForAdd(key);
    if (p) {
      p->value() = std::forward<ValueInput>(value);
      return true;
    }
    return add(p, std::forward<KeyInput>(key), std::forward<ValueInput>(value));
  }

  void remove(Ptr& p) { impl_.remove(p); }

  void remove(const Lookup& l) { impl_.remove(l); }
};

// A set with InlineEntries number of entries kept inline in an array.
//
// The T type must be zeroable as zeros are used as tombstone keys.
// The T type must have a default constructor.
//
// The API is very much like HashMap's.
template <typename T, size_t InlineEntries,
          typename HashPolicy = DefaultHasher<T>,
          typename AllocPolicy = TempAllocPolicy,
          typename KeyPolicy = detail::DefaultKeyPolicy<T>>
class InlineSet {
  using Set = HashSet<T, HashPolicy, AllocPolicy>;

  struct InlineEntry {
    T key;

    template <typename TInput>
    void update(TInput&& key) {
      this->key = std::forward<TInput>(key);
    }

    [[nodiscard]] bool moveTo(Set& set) { return set.putNew(std::move(key)); }
  };

  class Entry {
    using SetEntry = typename Set::Entry;

    SetEntry* setEntry_;
    InlineEntry* inlineEntry_;

   public:
    Entry() = default;

    explicit Entry(const SetEntry* setEntry)
        : setEntry_(const_cast<SetEntry*>(setEntry)), inlineEntry_(nullptr) {}

    explicit Entry(InlineEntry* inlineEntry)
        : setEntry_(nullptr), inlineEntry_(inlineEntry) {}

    operator T() const {
      MOZ_ASSERT(!!setEntry_ != !!inlineEntry_);
      if (setEntry_) {
        return *setEntry_;
      }
      return inlineEntry_->key;
    }
  };

  using Impl = detail::InlineTable<InlineEntry, Entry, Set, HashPolicy,
                                   AllocPolicy, KeyPolicy, InlineEntries>;

  Impl impl_;

 public:
  using Table = Set;
  using Ptr = typename Impl::Ptr;
  using AddPtr = typename Impl::AddPtr;
  using Range = typename Impl::Range;
  using Lookup = typename HashPolicy::Lookup;

  static const size_t SizeOfInlineEntries = Impl::SizeOfInlineEntries;

  explicit InlineSet(AllocPolicy a = AllocPolicy()) : impl_(std::move(a)) {}

  size_t count() const { return impl_.count(); }

  bool empty() const { return impl_.empty(); }

  void clear() { impl_.clear(); }

  Range all() const { return impl_.all(); }

  MOZ_ALWAYS_INLINE
  Ptr lookup(const Lookup& l) { return impl_.lookup(l); }

  MOZ_ALWAYS_INLINE
  bool has(const Lookup& l) const {
    return const_cast<InlineSet*>(this)->lookup(l).found();
  }

  MOZ_ALWAYS_INLINE
  AddPtr lookupForAdd(const Lookup& l) { return impl_.lookupForAdd(l); }

  template <typename TInput>
  [[nodiscard]] MOZ_ALWAYS_INLINE bool add(AddPtr& p, TInput&& key) {
    return impl_.add(p, std::forward<TInput>(key));
  }

  template <typename TInput>
  [[nodiscard]] bool put(TInput&& key) {
    AddPtr p = lookupForAdd(key);
    return p ? true : add(p, std::forward<TInput>(key));
  }

  void remove(Ptr& p) { impl_.remove(p); }

  void remove(const Lookup& l) { impl_.remove(l); }
};

}  // namespace js

#endif  // ds_InlineTable_h
