/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef ds_InlineTable_h
#define ds_InlineTable_h

#include "mozilla/Maybe.h"
#include "mozilla/Variant.h"

#include <utility>

#include "js/AllocPolicy.h"
#include "js/HashTable.h"

namespace js {

namespace detail {

template <typename InlineEntry, typename Entry, typename Table,
          typename HashPolicy, typename AllocPolicy, size_t InlineEntries>
class InlineTable : private AllocPolicy {
 private:
  using TablePtr = typename Table::Ptr;
  using TableAddPtr = typename Table::AddPtr;
  using TableRange = typename Table::Range;
  using Lookup = typename HashPolicy::Lookup;

  struct InlineArray {
    uint32_t count = 0;
    InlineEntry inl[InlineEntries];
  };
  mozilla::Variant<InlineArray, Table> data_{InlineArray()};
#ifdef DEBUG
  // Used to check that entries aren't added/removed while using Ptr/AddPtr or
  // Range. Similar to HashTable::mMutationCount.
  uint64_t mutationCount_ = 0;
#endif

#ifndef DEBUG
  // If this assertion fails, you should probably increase InlineEntries because
  // an extra inline entry could likely be added "for free".
  static_assert(sizeof(InlineArray) + sizeof(InlineEntry) >= sizeof(Table),
                "Space for additional inline elements in InlineTable?");
#endif

  InlineArray& inlineArray() { return data_.template as<InlineArray>(); }
  const InlineArray& inlineArray() const {
    return data_.template as<InlineArray>();
  }
  Table& table() { return data_.template as<Table>(); }
  const Table& table() const { return data_.template as<Table>(); }

  InlineEntry* inlineStart() {
    MOZ_ASSERT(!usingTable());
    return inlineArray().inl;
  }

  const InlineEntry* inlineStart() const {
    MOZ_ASSERT(!usingTable());
    return inlineArray().inl;
  }

  InlineEntry* inlineEnd() {
    MOZ_ASSERT(!usingTable());
    return inlineArray().inl + inlineArray().count;
  }

  const InlineEntry* inlineEnd() const {
    MOZ_ASSERT(!usingTable());
    return inlineArray().inl + inlineArray().count;
  }

  bool usingTable() const { return data_.template is<Table>(); }

  void bumpMutationCount() {
#ifdef DEBUG
    mutationCount_++;
#endif
  }

  [[nodiscard]] bool switchToTable() {
    MOZ_ASSERT(inlineArray().count == InlineEntries);

    Table table(*static_cast<AllocPolicy*>(this));

    // This is called before adding the next element, so reserve space for it
    // too.
    if (!table.reserve(InlineEntries + 1)) {
      return false;
    }

    InlineEntry* end = inlineStart() + InlineEntries;
    for (InlineEntry* it = inlineStart(); it != end; ++it) {
      // Note: don't use putNewInfallible because hashing can be fallible too.
      if (!it->moveTo(table)) {
        return false;
      }
    }

    MOZ_ASSERT(table.count() == InlineEntries);
    data_.template emplace<Table>(std::move(table));
    MOZ_ASSERT(usingTable());
    bumpMutationCount();
    return true;
  }

 public:
  static const size_t SizeOfInlineEntries = sizeof(InlineEntry) * InlineEntries;

  explicit InlineTable(AllocPolicy a = AllocPolicy())
      : AllocPolicy(std::move(a)) {}

  class Ptr {
    friend class InlineTable;

    MOZ_INIT_OUTSIDE_CTOR Entry entry_;
    MOZ_INIT_OUTSIDE_CTOR TablePtr tablePtr_;
    MOZ_INIT_OUTSIDE_CTOR InlineEntry* inlPtr_;
    MOZ_INIT_OUTSIDE_CTOR bool isInlinePtr_;
#ifdef DEBUG
    uint64_t mutationCount_ = 0xbadbad;
#endif

    Ptr(const InlineTable& table, TablePtr p)
        : entry_(p.found() ? &*p : nullptr), tablePtr_(p), isInlinePtr_(false) {
#ifdef DEBUG
      mutationCount_ = table.mutationCount_;
#endif
    }

    Ptr(const InlineTable& table, InlineEntry* inlineEntry)
        : entry_(inlineEntry), inlPtr_(inlineEntry), isInlinePtr_(true) {
#ifdef DEBUG
      mutationCount_ = table.mutationCount_;
#endif
    }

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

    MOZ_INIT_OUTSIDE_CTOR Entry entry_;
    MOZ_INIT_OUTSIDE_CTOR TableAddPtr tableAddPtr_;
    MOZ_INIT_OUTSIDE_CTOR InlineEntry* inlAddPtr_;
    MOZ_INIT_OUTSIDE_CTOR bool isInlinePtr_;
    // Indicates whether inlAddPtr is a found result or an add pointer.
    MOZ_INIT_OUTSIDE_CTOR bool inlPtrFound_;
#ifdef DEBUG
    uint64_t mutationCount_ = 0xbadbad;
#endif

    AddPtr(const InlineTable& table, InlineEntry* ptr, bool found)
        : entry_(ptr),
          inlAddPtr_(ptr),
          isInlinePtr_(true),
          inlPtrFound_(found) {
#ifdef DEBUG
      mutationCount_ = table.mutationCount_;
#endif
    }

    AddPtr(const InlineTable& table, const TableAddPtr& p)
        : entry_(p.found() ? &*p : nullptr),
          tableAddPtr_(p),
          isInlinePtr_(false) {
#ifdef DEBUG
      mutationCount_ = table.mutationCount_;
#endif
    }

   public:
    AddPtr() = default;

    bool found() const {
      return isInlinePtr_ ? inlPtrFound_ : tableAddPtr_.found();
    }

    explicit operator bool() const { return found(); }

    Entry& operator*() {
      MOZ_ASSERT(found());
      return entry_;
    }

    Entry* operator->() {
      MOZ_ASSERT(found());
      return &entry_;
    }
  };

  size_t count() const {
    return usingTable() ? table().count() : inlineArray().count;
  }

  bool empty() const {
    return usingTable() ? table().empty() : !inlineArray().count;
  }

  void clear() {
    data_.template emplace<InlineArray>();
    bumpMutationCount();
  }

  MOZ_ALWAYS_INLINE
  Ptr lookup(const Lookup& l) {
    if (usingTable()) {
      return Ptr(*this, table().lookup(l));
    }

    InlineEntry* end = inlineEnd();
    for (InlineEntry* it = inlineStart(); it != end; ++it) {
      if (HashPolicy::match(it->key, l)) {
        return Ptr(*this, it);
      }
    }

    return Ptr(*this, nullptr);
  }

  MOZ_ALWAYS_INLINE
  AddPtr lookupForAdd(const Lookup& l) {
    if (usingTable()) {
      return AddPtr(*this, table().lookupForAdd(l));
    }

    InlineEntry* end = inlineEnd();
    for (InlineEntry* it = inlineStart(); it != end; ++it) {
      if (HashPolicy::match(it->key, l)) {
        return AddPtr(*this, it, true);
      }
    }

    // The add pointer that's returned here may indicate the limit entry of
    // the linear space, in which case the |add| operation will initialize
    // the table if necessary and add the entry there.
    return AddPtr(*this, inlineEnd(), false);
  }

  template <typename KeyInput, typename... Args>
  [[nodiscard]] MOZ_ALWAYS_INLINE bool add(AddPtr& p, KeyInput&& key,
                                           Args&&... args) {
    MOZ_ASSERT(!p);
    MOZ_ASSERT(p.mutationCount_ == mutationCount_);

    if (p.isInlinePtr_) {
      InlineEntry* addPtr = p.inlAddPtr_;
      MOZ_ASSERT(addPtr == inlineEnd());

      // Switching to table mode before we add this pointer.
      if (addPtr == inlineStart() + InlineEntries) {
        if (!switchToTable()) {
          return false;
        }
        if (!table().putNew(std::forward<KeyInput>(key),
                            std::forward<Args>(args)...)) {
          return false;
        }
        bumpMutationCount();
        return true;
      }

      MOZ_ASSERT(!p.found());
      MOZ_ASSERT(uintptr_t(inlineEnd()) == uintptr_t(p.inlAddPtr_));

      if (!this->checkSimulatedOOM()) {
        return false;
      }

      addPtr->update(std::forward<KeyInput>(key), std::forward<Args>(args)...);
      inlineArray().count++;
      bumpMutationCount();
      return true;
    }

    if (!table().add(p.tableAddPtr_, std::forward<KeyInput>(key),
                     std::forward<Args>(args)...)) {
      return false;
    }
    bumpMutationCount();
    return true;
  }

  void remove(Ptr& p) {
    MOZ_ASSERT(p);
    MOZ_ASSERT(p.mutationCount_ == mutationCount_);
    if (p.isInlinePtr_) {
      InlineArray& arr = inlineArray();
      MOZ_ASSERT(arr.count > 0);
      InlineEntry* last = &arr.inl[arr.count - 1];
      MOZ_ASSERT(p.inlPtr_ <= last);
      if (p.inlPtr_ != last) {
        // Removing an entry that's not the last one. Move the last entry.
        *p.inlPtr_ = std::move(*last);
      }
      arr.count--;
    } else {
      MOZ_ASSERT(usingTable());
      table().remove(p.tablePtr_);
    }
    bumpMutationCount();
  }

  void remove(const Lookup& l) {
    if (Ptr p = lookup(l)) {
      remove(p);
    }
  }

  class Range {
    friend class InlineTable;

    mozilla::Maybe<TableRange> tableRange_;  // `Nothing` if `isInline_==true`
    InlineEntry* cur_;
    InlineEntry* end_;
    bool isInline_;
#ifdef DEBUG
    const InlineTable* table_ = nullptr;
    uint64_t mutationCount_ = 0xbadbad;
#endif

    Range(const InlineTable& table, TableRange r)
        : tableRange_(mozilla::Some(r)),
          cur_(nullptr),
          end_(nullptr),
          isInline_(false) {
      MOZ_ASSERT(!isInlineRange());
#ifdef DEBUG
      table_ = &table;
      mutationCount_ = table.mutationCount_;
#endif
    }

    Range(const InlineTable& table, const InlineEntry* begin,
          const InlineEntry* end)
        : tableRange_(mozilla::Nothing()),
          cur_(const_cast<InlineEntry*>(begin)),
          end_(const_cast<InlineEntry*>(end)),
          isInline_(true) {
      MOZ_ASSERT(isInlineRange());
#ifdef DEBUG
      table_ = &table;
      mutationCount_ = table.mutationCount_;
#endif
    }

    bool assertInlineRangeInvariants() const {
      MOZ_ASSERT(uintptr_t(cur_) <= uintptr_t(end_));
      return true;
    }

    bool isInlineRange() const {
      MOZ_ASSERT_IF(isInline_, assertInlineRangeInvariants());
      return isInline_;
    }

    void bumpCurPtr() {
      MOZ_ASSERT(isInlineRange());
      cur_++;
    }

   public:
    bool empty() const {
      MOZ_ASSERT(table_->mutationCount_ == mutationCount_);
      return isInlineRange() ? cur_ == end_ : tableRange_->empty();
    }

    Entry front() {
      MOZ_ASSERT(!empty());
      MOZ_ASSERT(table_->mutationCount_ == mutationCount_);
      if (isInlineRange()) {
        return Entry(cur_);
      }
      return Entry(&tableRange_->front());
    }

    void popFront() {
      MOZ_ASSERT(!empty());
      MOZ_ASSERT(table_->mutationCount_ == mutationCount_);
      if (isInlineRange()) {
        bumpCurPtr();
      } else {
        tableRange_->popFront();
      }
    }
  };

  Range all() const {
    return usingTable() ? Range(*this, table().all())
                        : Range(*this, inlineStart(), inlineEnd());
  }
};

}  // namespace detail

// A map with InlineEntries number of entries kept inline in an array.
//
// The Value type must have a default constructor.
//
// The API is very much like HashMap's.
template <typename Key, typename Value, size_t InlineEntries,
          typename HashPolicy = DefaultHasher<Key>,
          typename AllocPolicy = TempAllocPolicy>
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
                                   AllocPolicy, InlineEntries>;

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
// The T type must have a default constructor.
//
// The API is very much like HashSet's.
template <typename T, size_t InlineEntries,
          typename HashPolicy = DefaultHasher<T>,
          typename AllocPolicy = TempAllocPolicy>
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
                                   AllocPolicy, InlineEntries>;

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
