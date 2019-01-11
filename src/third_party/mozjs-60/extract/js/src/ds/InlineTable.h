/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef ds_InlineTable_h
#define ds_InlineTable_h

#include "mozilla/Move.h"

#include "js/AllocPolicy.h"
#include "js/HashTable.h"

namespace js {

namespace detail {

template <typename InlineEntry,
          typename Entry,
          typename Table,
          typename HashPolicy,
          typename AllocPolicy,
          size_t InlineEntries>
class InlineTable
{
  private:
    using TablePtr    = typename Table::Ptr;
    using TableAddPtr = typename Table::AddPtr;
    using TableRange  = typename Table::Range;
    using Lookup      = typename HashPolicy::Lookup;

    size_t      inlNext_;
    size_t      inlCount_;
    InlineEntry inl_[InlineEntries];
    Table       table_;

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

    bool usingTable() const {
        return inlNext_ > InlineEntries;
    }

    MOZ_MUST_USE bool switchToTable() {
        MOZ_ASSERT(inlNext_ == InlineEntries);

        if (table_.initialized()) {
            table_.clear();
        } else {
            if (!table_.init(count()))
                return false;
            MOZ_ASSERT(table_.initialized());
        }

        InlineEntry* end = inlineEnd();
        for (InlineEntry* it = inlineStart(); it != end; ++it) {
            if (it->key && !it->moveTo(table_))
                return false;
        }

        inlNext_ = InlineEntries + 1;
        MOZ_ASSERT(table_.count() == inlCount_);
        MOZ_ASSERT(usingTable());
        return true;
    }

    MOZ_NEVER_INLINE
    MOZ_MUST_USE bool switchAndAdd(const InlineEntry& entry) {
        if (!switchToTable())
            return false;

        return entry.putNew(table_);
    }

  public:
    static const size_t SizeOfInlineEntries = sizeof(InlineEntry) * InlineEntries;

    explicit InlineTable(AllocPolicy a = AllocPolicy())
      : inlNext_(0),
        inlCount_(0),
        table_(a)
    { }

    class Ptr
    {
        friend class InlineTable;

      protected:
        MOZ_INIT_OUTSIDE_CTOR Entry        entry_;
        MOZ_INIT_OUTSIDE_CTOR TablePtr     tablePtr_;
        MOZ_INIT_OUTSIDE_CTOR InlineEntry* inlPtr_;
        MOZ_INIT_OUTSIDE_CTOR bool         isInlinePtr_;

        explicit Ptr(TablePtr p)
          : entry_(p.found() ? &*p : nullptr),
            tablePtr_(p),
            isInlinePtr_(false)
        { }

        explicit Ptr(InlineEntry* inlineEntry)
          : entry_(inlineEntry),
            inlPtr_(inlineEntry),
            isInlinePtr_(true)
        { }

        void operator==(const Ptr& other);

      public:
        // Leaves Ptr uninitialized.
        Ptr() {
#ifdef DEBUG
            inlPtr_ = (InlineEntry*) 0xbad;
            isInlinePtr_ = true;
#endif
        }

        // Default copy constructor works for this structure.

        bool found() const {
            return isInlinePtr_ ? bool(inlPtr_) : tablePtr_.found();
        }

        explicit operator bool() const {
            return found();
        }

        bool operator==(const Ptr& other) const {
            MOZ_ASSERT(found() && other.found());
            if (isInlinePtr_ != other.isInlinePtr_)
                return false;
            if (isInlinePtr_)
                return inlPtr_ == other.inlPtr_;
            return tablePtr_ == other.tablePtr_;
        }

        bool operator!=(const Ptr& other) const {
            return !(*this == other);
        }

        Entry& operator*() {
            MOZ_ASSERT(found());
            return entry_;
        }

        Entry* operator->() {
            MOZ_ASSERT(found());
            return &entry_;
        }
    };

    class AddPtr
    {
        friend class InlineTable;

      protected:
        MOZ_INIT_OUTSIDE_CTOR Entry        entry_;
        MOZ_INIT_OUTSIDE_CTOR TableAddPtr  tableAddPtr_;
        MOZ_INIT_OUTSIDE_CTOR InlineEntry* inlAddPtr_;
        MOZ_INIT_OUTSIDE_CTOR bool         isInlinePtr_;
        // Indicates whether inlAddPtr is a found result or an add pointer.
        MOZ_INIT_OUTSIDE_CTOR bool         inlPtrFound_;

        AddPtr(InlineEntry* ptr, bool found)
          : entry_(ptr),
            inlAddPtr_(ptr),
            isInlinePtr_(true),
            inlPtrFound_(found)
        {}

        explicit AddPtr(const TableAddPtr& p)
          : entry_(p.found() ? &*p : nullptr),
            tableAddPtr_(p),
            isInlinePtr_(false)
        { }

      public:
        AddPtr() {}

        bool found() const {
            return isInlinePtr_ ? inlPtrFound_ : tableAddPtr_.found();
        }

        explicit operator bool() const {
            return found();
        }

        bool operator==(const AddPtr& other) const {
            MOZ_ASSERT(found() && other.found());
            if (isInlinePtr_ != other.isInlinePtr_)
                return false;
            if (isInlinePtr_)
                return inlAddPtr_ == other.inlAddPtr_;
            return tableAddPtr_ == other.tableAddPtr_;
        }

        bool operator!=(const AddPtr& other) const {
            return !(*this == other);
        }

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
        return usingTable() ? table_.count() : inlCount_;
    }

    bool empty() const {
        return usingTable() ? table_.empty() : !inlCount_;
    }

    void clear() {
        inlNext_ = 0;
        inlCount_ = 0;
    }

    MOZ_ALWAYS_INLINE
    Ptr lookup(const Lookup& l) {
        MOZ_ASSERT(keyNonZero(l));

        if (usingTable())
            return Ptr(table_.lookup(l));

        InlineEntry* end = inlineEnd();
        for (InlineEntry* it = inlineStart(); it != end; ++it) {
            if (it->key && HashPolicy::match(it->key, l))
                return Ptr(it);
        }

        return Ptr(nullptr);
    }

    MOZ_ALWAYS_INLINE
    AddPtr lookupForAdd(const Lookup& l) {
        MOZ_ASSERT(keyNonZero(l));

        if (usingTable())
            return AddPtr(table_.lookupForAdd(l));

        InlineEntry* end = inlineEnd();
        for (InlineEntry* it = inlineStart(); it != end; ++it) {
            if (it->key && HashPolicy::match(it->key, l))
                return AddPtr(it, true);
        }

        // The add pointer that's returned here may indicate the limit entry of
        // the linear space, in which case the |add| operation will initialize
        // the table if necessary and add the entry there.
        return AddPtr(inlineEnd(), false);
    }

    template <typename KeyInput,
              typename... Args>
    MOZ_ALWAYS_INLINE
    MOZ_MUST_USE bool add(AddPtr& p, KeyInput&& key, Args&&... args) {
        MOZ_ASSERT(!p);
        MOZ_ASSERT(keyNonZero(key));

        if (p.isInlinePtr_) {
            InlineEntry* addPtr = p.inlAddPtr_;
            MOZ_ASSERT(addPtr == inlineEnd());

            // Switching to table mode before we add this pointer.
            if (addPtr == inlineStart() + InlineEntries) {
                if (!switchToTable())
                    return false;
                return table_.putNew(mozilla::Forward<KeyInput>(key),
                                     mozilla::Forward<Args>(args)...);
            }

            MOZ_ASSERT(!p.found());
            MOZ_ASSERT(uintptr_t(inlineEnd()) == uintptr_t(p.inlAddPtr_));
            addPtr->update(mozilla::Forward<KeyInput>(key),
                           mozilla::Forward<Args>(args)...);
            ++inlCount_;
            ++inlNext_;
            return true;
        }

        return table_.add(p.tableAddPtr_,
                          mozilla::Forward<KeyInput>(key),
                          mozilla::Forward<Args>(args)...);
    }

    void remove(Ptr& p) {
        MOZ_ASSERT(p);
        if (p.isInlinePtr_) {
            MOZ_ASSERT(inlCount_ > 0);
            MOZ_ASSERT(p.inlPtr_->key != nullptr);
            p.inlPtr_->key = nullptr;
            --inlCount_;
            return;
        }
        MOZ_ASSERT(table_.initialized() && usingTable());
        table_.remove(p.tablePtr_);
    }

    void remove(const Lookup& l) {
        if (Ptr p = lookup(l))
            remove(p);
    }

    class Range
    {
        friend class InlineTable;

      protected:
        TableRange   tableRange_;
        InlineEntry* cur_;
        InlineEntry* end_;
        bool         isInline_;

        explicit Range(TableRange r)
          : cur_(nullptr),
            end_(nullptr),
            isInline_(false)
        {
            tableRange_ = r;
            MOZ_ASSERT(!isInlineRange());
        }

        Range(const InlineEntry* begin, const InlineEntry* end)
          : cur_(const_cast<InlineEntry*>(begin)),
            end_(const_cast<InlineEntry*>(end)),
            isInline_(true)
        {
            advancePastNulls(cur_);
            MOZ_ASSERT(isInlineRange());
        }

        bool assertInlineRangeInvariants() const {
            MOZ_ASSERT(uintptr_t(cur_) <= uintptr_t(end_));
            MOZ_ASSERT_IF(cur_ != end_, cur_->key != nullptr);
            return true;
        }

        bool isInlineRange() const {
            MOZ_ASSERT_IF(isInline_, assertInlineRangeInvariants());
            return isInline_;
        }

        void advancePastNulls(InlineEntry* begin) {
            InlineEntry* newCur = begin;
            while (newCur < end_ && nullptr == newCur->key)
                ++newCur;
            MOZ_ASSERT(uintptr_t(newCur) <= uintptr_t(end_));
            cur_ = newCur;
        }

        void bumpCurPtr() {
            MOZ_ASSERT(isInlineRange());
            advancePastNulls(cur_ + 1);
        }

      public:
        bool empty() const {
            return isInlineRange() ? cur_ == end_ : tableRange_.empty();
        }

        Entry front() {
            MOZ_ASSERT(!empty());
            if (isInlineRange())
                return Entry(cur_);
            return Entry(&tableRange_.front());
        }

        void popFront() {
            MOZ_ASSERT(!empty());
            if (isInlineRange())
                bumpCurPtr();
            else
                tableRange_.popFront();
        }
    };

    Range all() const {
        return usingTable() ? Range(table_.all()) : Range(inlineStart(), inlineEnd());
    }
};

} // namespace detail

// A map with InlineEntries number of entries kept inline in an array.
//
// The Key type must be zeroable as zeros are used as tombstone keys.
// The Value type must have a default constructor.
//
// The API is very much like HashMap's.
template <typename Key,
          typename Value,
          size_t InlineEntries,
          typename HashPolicy = DefaultHasher<Key>,
          typename AllocPolicy = TempAllocPolicy>
class InlineMap
{
    using Map = HashMap<Key, Value, HashPolicy, AllocPolicy>;

    struct InlineEntry
    {
        Key   key;
        Value value;

        template <typename KeyInput, typename ValueInput>
        void update(KeyInput&& key, ValueInput&& value) {
            this->key = mozilla::Forward<KeyInput>(key);
            this->value = mozilla::Forward<ValueInput>(value);
        }

        MOZ_MUST_USE bool moveTo(Map& map) {
            return map.putNew(mozilla::Move(key), mozilla::Move(value));
        }
    };

    class Entry
    {
        using MapEntry = typename Map::Entry;

        MapEntry*    mapEntry_;
        InlineEntry* inlineEntry_;

      public:
        Entry() = default;

        explicit Entry(MapEntry* mapEntry)
          : mapEntry_(mapEntry),
            inlineEntry_(nullptr)
        { }

        explicit Entry(InlineEntry* inlineEntry)
          : mapEntry_(nullptr),
            inlineEntry_(inlineEntry)
        { }

        const Key& key() const {
            MOZ_ASSERT(!!mapEntry_ != !!inlineEntry_);
            if (mapEntry_)
                return mapEntry_->key();
            return inlineEntry_->key;
        }

        Value& value() {
            MOZ_ASSERT(!!mapEntry_ != !!inlineEntry_);
            if (mapEntry_)
                return mapEntry_->value();
            return inlineEntry_->value;
        }
    };

    using Impl = detail::InlineTable<InlineEntry, Entry,
                                     Map, HashPolicy, AllocPolicy,
                                     InlineEntries>;

    Impl impl_;

  public:
    using Table  = Map;
    using Ptr    = typename Impl::Ptr;
    using AddPtr = typename Impl::AddPtr;
    using Range  = typename Impl::Range;
    using Lookup = typename HashPolicy::Lookup;

    static const size_t SizeOfInlineEntries = Impl::SizeOfInlineEntries;

    explicit InlineMap(AllocPolicy a = AllocPolicy())
      : impl_(a)
    { }

    size_t count() const {
        return impl_.count();
    }

    bool empty() const {
        return impl_.empty();
    }

    void clear() {
        impl_.clear();
    }

    Range all() const {
        return impl_.all();
    }

    MOZ_ALWAYS_INLINE
    Ptr lookup(const Lookup& l) {
        return impl_.lookup(l);
    }

    MOZ_ALWAYS_INLINE
    bool has(const Lookup& l) const {
        return const_cast<InlineMap*>(this)->lookup(l).found();
    }

    MOZ_ALWAYS_INLINE
    AddPtr lookupForAdd(const Lookup& l) {
        return impl_.lookupForAdd(l);
    }

    template <typename KeyInput, typename ValueInput>
    MOZ_ALWAYS_INLINE
    MOZ_MUST_USE bool add(AddPtr& p, KeyInput&& key, ValueInput&& value) {
        return impl_.add(p, mozilla::Forward<KeyInput>(key), mozilla::Forward<ValueInput>(value));
    }

    template <typename KeyInput, typename ValueInput>
    MOZ_MUST_USE bool put(KeyInput&& key, ValueInput&& value) {
        AddPtr p = lookupForAdd(key);
        if (p) {
            p->value() = mozilla::Forward<ValueInput>(value);
            return true;
        }
        return add(p, mozilla::Forward<KeyInput>(key), mozilla::Forward<ValueInput>(value));
    }

    void remove(Ptr& p) {
        impl_.remove(p);
    }

    void remove(const Lookup& l) {
        impl_.remove(l);
    }
};

// A set with InlineEntries number of entries kept inline in an array.
//
// The T type must be zeroable as zeros are used as tombstone keys.
// The T type must have a default constructor.
//
// The API is very much like HashMap's.
template <typename T,
          size_t InlineEntries,
          typename HashPolicy = DefaultHasher<T>,
          typename AllocPolicy = TempAllocPolicy>
class InlineSet
{
    using Set = HashSet<T, HashPolicy, AllocPolicy>;

    struct InlineEntry
    {
        T key;

        template <typename TInput>
        void update(TInput&& key) {
            this->key = mozilla::Forward<TInput>(key);
        }

        MOZ_MUST_USE bool moveTo(Set& set) {
            return set.putNew(mozilla::Move(key));
        }
    };

    class Entry
    {
        using SetEntry = typename Set::Entry;

        SetEntry*    setEntry_;
        InlineEntry* inlineEntry_;

      public:
        Entry() = default;

        explicit Entry(const SetEntry* setEntry)
          : setEntry_(const_cast<SetEntry*>(setEntry)),
            inlineEntry_(nullptr)
        { }

        explicit Entry(InlineEntry* inlineEntry)
          : setEntry_(nullptr),
            inlineEntry_(inlineEntry)
        { }

        operator T() const {
            MOZ_ASSERT(!!setEntry_ != !!inlineEntry_);
            if (setEntry_)
                return *setEntry_;
            return inlineEntry_->key;
        }
    };

    using Impl = detail::InlineTable<InlineEntry, Entry,
                                     Set, HashPolicy, AllocPolicy,
                                     InlineEntries>;

    Impl impl_;

  public:
    using Table  = Set;
    using Ptr    = typename Impl::Ptr;
    using AddPtr = typename Impl::AddPtr;
    using Range  = typename Impl::Range;
    using Lookup = typename HashPolicy::Lookup;

    static const size_t SizeOfInlineEntries = Impl::SizeOfInlineEntries;

    explicit InlineSet(AllocPolicy a = AllocPolicy())
      : impl_(a)
    { }

    size_t count() const {
        return impl_.count();
    }

    bool empty() const {
        return impl_.empty();
    }

    void clear() {
        impl_.clear();
    }

    Range all() const {
        return impl_.all();
    }

    MOZ_ALWAYS_INLINE
    Ptr lookup(const Lookup& l) {
        return impl_.lookup(l);
    }

    MOZ_ALWAYS_INLINE
    bool has(const Lookup& l) const {
        return const_cast<InlineSet*>(this)->lookup(l).found();
    }

    MOZ_ALWAYS_INLINE
    AddPtr lookupForAdd(const Lookup& l) {
        return impl_.lookupForAdd(l);
    }

    template <typename TInput>
    MOZ_ALWAYS_INLINE
    MOZ_MUST_USE bool add(AddPtr& p, TInput&& key) {
        return impl_.add(p, mozilla::Forward<TInput>(key));
    }

    template <typename TInput>
    MOZ_MUST_USE bool put(TInput&& key) {
        AddPtr p = lookupForAdd(key);
        return p ? true : add(p, mozilla::Forward<TInput>(key));
    }

    void remove(Ptr& p) {
        impl_.remove(p);
    }

    void remove(const Lookup& l) {
        impl_.remove(l);
    }
};

} // namespace js

#endif // ds_InlineTable_h
