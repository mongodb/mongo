/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_NameCollections_h
#define frontend_NameCollections_h

#include "mozilla/Assertions.h"  // MOZ_ASSERT
#include "mozilla/Attributes.h"  // MOZ_IMPLICIT

#include <stddef.h>     // size_t
#include <stdint.h>     // uint32_t, uint64_t
#include <type_traits>  // std::{true_type, false_type, is_trivial_v, is_trivially_copyable_v, is_trivially_destructible_v}
#include <utility>      // std::forward

#include "ds/InlineTable.h"              // InlineMap, DefaultKeyPolicy
#include "frontend/NameAnalysisTypes.h"  // AtomVector, FunctionBoxVector
#include "frontend/ParserAtom.h"  // TaggedParserAtomIndex, TrivialTaggedParserAtomIndex
#include "frontend/TaggedParserAtomIndexHasher.h"  // TrivialTaggedParserAtomIndexHasher
#include "js/AllocPolicy.h"  // SystemAllocPolicy, ReportOutOfMemory
#include "js/Utility.h"      // js_new, js_delete
#include "js/Vector.h"       // Vector

namespace js::frontend {

class FunctionBox;

// A pool of recyclable containers for use in the frontend. The Parser and
// BytecodeEmitter create many maps for name analysis that are short-lived
// (i.e., for the duration of parsing or emitting a lexical scope). Making
// them recyclable cuts down significantly on allocator churn.
template <typename RepresentativeCollection, typename ConcreteCollectionPool>
class CollectionPool {
  using RecyclableCollections = Vector<void*, 32, SystemAllocPolicy>;

  RecyclableCollections all_;
  RecyclableCollections recyclable_;

  static RepresentativeCollection* asRepresentative(void* p) {
    return reinterpret_cast<RepresentativeCollection*>(p);
  }

  RepresentativeCollection* allocate() {
    size_t newAllLength = all_.length() + 1;
    if (!all_.reserve(newAllLength) || !recyclable_.reserve(newAllLength)) {
      return nullptr;
    }

    RepresentativeCollection* collection = js_new<RepresentativeCollection>();
    if (collection) {
      all_.infallibleAppend(collection);
    }
    return collection;
  }

 public:
  ~CollectionPool() { purgeAll(); }

  void purgeAll() {
    void** end = all_.end();
    for (void** it = all_.begin(); it != end; ++it) {
      js_delete(asRepresentative(*it));
    }

    all_.clearAndFree();
    recyclable_.clearAndFree();
  }

  // Fallibly aquire one of the supported collection types from the pool.
  template <typename Collection>
  Collection* acquire(FrontendContext* fc) {
    ConcreteCollectionPool::template assertInvariants<Collection>();

    RepresentativeCollection* collection;
    if (recyclable_.empty()) {
      collection = allocate();
      if (!collection) {
        ReportOutOfMemory(fc);
      }
    } else {
      collection = asRepresentative(recyclable_.popCopy());
      collection->clear();
    }
    return reinterpret_cast<Collection*>(collection);
  }

  // Release a collection back to the pool.
  template <typename Collection>
  void release(Collection** collection) {
    ConcreteCollectionPool::template assertInvariants<Collection>();
    MOZ_ASSERT(*collection);

#ifdef DEBUG
    bool ok = false;
    // Make sure the collection is in |all_| but not already in |recyclable_|.
    for (void** it = all_.begin(); it != all_.end(); ++it) {
      if (*it == *collection) {
        ok = true;
        break;
      }
    }
    MOZ_ASSERT(ok);
    for (void** it = recyclable_.begin(); it != recyclable_.end(); ++it) {
      MOZ_ASSERT(*it != *collection);
    }
#endif

    MOZ_ASSERT(recyclable_.length() < all_.length());
    // Reserved in allocateFresh.
    recyclable_.infallibleAppend(*collection);
    *collection = nullptr;
  }
};

template <typename Wrapped>
struct RecyclableAtomMapValueWrapper {
  using WrappedType = Wrapped;

  union {
    Wrapped wrapped;
    uint64_t dummy;
  };

  static void assertInvariant() {
    static_assert(sizeof(Wrapped) <= sizeof(uint64_t),
                  "Can only recycle atom maps with values smaller than uint64");
  }

  RecyclableAtomMapValueWrapper() : dummy(0) { assertInvariant(); }

  MOZ_IMPLICIT RecyclableAtomMapValueWrapper(Wrapped w) : wrapped(w) {
    assertInvariant();
  }

  MOZ_IMPLICIT operator Wrapped&() { return wrapped; }

  MOZ_IMPLICIT operator Wrapped&() const { return wrapped; }

  Wrapped* operator->() { return &wrapped; }

  const Wrapped* operator->() const { return &wrapped; }
};

template <typename MapValue>
using RecyclableNameMapBase =
    InlineMap<TrivialTaggedParserAtomIndex,
              RecyclableAtomMapValueWrapper<MapValue>, 24,
              TrivialTaggedParserAtomIndexHasher, SystemAllocPolicy>;

// Define wrapper methods to accept TaggedParserAtomIndex.
template <typename MapValue>
class RecyclableNameMap : public RecyclableNameMapBase<MapValue> {
  using Base = RecyclableNameMapBase<MapValue>;

 public:
  template <typename... Args>
  [[nodiscard]] MOZ_ALWAYS_INLINE bool add(typename Base::AddPtr& p,
                                           const TaggedParserAtomIndex& key,
                                           Args&&... args) {
    return Base::add(p, TrivialTaggedParserAtomIndex::from(key),
                     std::forward<Args>(args)...);
  }

  MOZ_ALWAYS_INLINE
  typename Base::Ptr lookup(const TaggedParserAtomIndex& l) {
    return Base::lookup(TrivialTaggedParserAtomIndex::from(l));
  }

  MOZ_ALWAYS_INLINE
  typename Base::AddPtr lookupForAdd(const TaggedParserAtomIndex& l) {
    return Base::lookupForAdd(TrivialTaggedParserAtomIndex::from(l));
  }
};

using DeclaredNameMap = RecyclableNameMap<DeclaredNameInfo>;
using NameLocationMap = RecyclableNameMap<NameLocation>;
// Cannot use GCThingIndex here because it's not trivial type.
using AtomIndexMap = RecyclableNameMap<uint32_t>;

template <typename RepresentativeTable>
class InlineTablePool
    : public CollectionPool<RepresentativeTable,
                            InlineTablePool<RepresentativeTable>> {
  template <typename>
  struct IsRecyclableAtomMapValueWrapper : std::false_type {};

  template <typename T>
  struct IsRecyclableAtomMapValueWrapper<RecyclableAtomMapValueWrapper<T>>
      : std::true_type {};

 public:
  template <typename Table>
  static void assertInvariants() {
    static_assert(
        Table::SizeOfInlineEntries == RepresentativeTable::SizeOfInlineEntries,
        "Only tables with the same size for inline entries are usable in the "
        "pool.");

    using EntryType = typename Table::Table::Entry;
    using KeyType = typename EntryType::KeyType;
    using ValueType = typename EntryType::ValueType;

    static_assert(IsRecyclableAtomMapValueWrapper<ValueType>::value,
                  "Please adjust the static assertions below if you need to "
                  "support other types than RecyclableAtomMapValueWrapper");

    using WrappedType = typename ValueType::WrappedType;

    // We can't directly check |std::is_trivial<EntryType>|, because neither
    // mozilla::HashMapEntry nor IsRecyclableAtomMapValueWrapper are trivially
    // default constructible. Instead we check that the key and the unwrapped
    // value are trivial and additionally ensure that the entry itself is
    // trivially copyable and destructible.

    static_assert(std::is_trivial_v<KeyType>,
                  "Only tables with trivial keys are usable in the pool.");
    static_assert(std::is_trivial_v<WrappedType>,
                  "Only tables with trivial values are usable in the pool.");

    static_assert(
        std::is_trivially_copyable_v<EntryType>,
        "Only tables with trivially copyable entries are usable in the pool.");
    static_assert(std::is_trivially_destructible_v<EntryType>,
                  "Only tables with trivially destructible entries are usable "
                  "in the pool.");
  }
};

template <typename RepresentativeVector>
class VectorPool : public CollectionPool<RepresentativeVector,
                                         VectorPool<RepresentativeVector>> {
 public:
  template <typename Vector>
  static void assertInvariants() {
    static_assert(
        Vector::sMaxInlineStorage == RepresentativeVector::sMaxInlineStorage,
        "Only vectors with the same size for inline entries are usable in the "
        "pool.");

    using ElementType = typename Vector::ElementType;

    static_assert(std::is_trivial_v<ElementType>,
                  "Only vectors of trivial values are usable in the pool.");
    static_assert(std::is_trivially_destructible_v<ElementType>,
                  "Only vectors of trivially destructible values are usable in "
                  "the pool.");

    static_assert(
        sizeof(ElementType) ==
            sizeof(typename RepresentativeVector::ElementType),
        "Only vectors with same-sized elements are usable in the pool.");
  }
};

using AtomVector = Vector<TrivialTaggedParserAtomIndex, 24, SystemAllocPolicy>;

using FunctionBoxVector = Vector<FunctionBox*, 24, SystemAllocPolicy>;

class NameCollectionPool {
  InlineTablePool<AtomIndexMap> mapPool_;
  VectorPool<AtomVector> atomVectorPool_;
  VectorPool<FunctionBoxVector> functionBoxVectorPool_;
  uint32_t activeCompilations_;

 public:
  NameCollectionPool() : activeCompilations_(0) {}

  bool hasActiveCompilation() const { return activeCompilations_ != 0; }

  void addActiveCompilation() { activeCompilations_++; }

  void removeActiveCompilation() {
    MOZ_ASSERT(hasActiveCompilation());
    activeCompilations_--;
  }

  template <typename Map>
  Map* acquireMap(FrontendContext* fc) {
    MOZ_ASSERT(hasActiveCompilation());
    return mapPool_.acquire<Map>(fc);
  }

  template <typename Map>
  void releaseMap(Map** map) {
    MOZ_ASSERT(hasActiveCompilation());
    MOZ_ASSERT(map);
    if (*map) {
      mapPool_.release(map);
    }
  }

  template <typename Vector>
  inline Vector* acquireVector(FrontendContext* fc);

  template <typename Vector>
  inline void releaseVector(Vector** vec);

  void purge() {
    if (!hasActiveCompilation()) {
      mapPool_.purgeAll();
      atomVectorPool_.purgeAll();
      functionBoxVectorPool_.purgeAll();
    }
  }
};

template <>
inline AtomVector* NameCollectionPool::acquireVector<AtomVector>(
    FrontendContext* fc) {
  MOZ_ASSERT(hasActiveCompilation());
  return atomVectorPool_.acquire<AtomVector>(fc);
}

template <>
inline void NameCollectionPool::releaseVector<AtomVector>(AtomVector** vec) {
  MOZ_ASSERT(hasActiveCompilation());
  MOZ_ASSERT(vec);
  if (*vec) {
    atomVectorPool_.release(vec);
  }
}

template <>
inline FunctionBoxVector* NameCollectionPool::acquireVector<FunctionBoxVector>(
    FrontendContext* fc) {
  MOZ_ASSERT(hasActiveCompilation());
  return functionBoxVectorPool_.acquire<FunctionBoxVector>(fc);
}

template <>
inline void NameCollectionPool::releaseVector<FunctionBoxVector>(
    FunctionBoxVector** vec) {
  MOZ_ASSERT(hasActiveCompilation());
  MOZ_ASSERT(vec);
  if (*vec) {
    functionBoxVectorPool_.release(vec);
  }
}

template <typename T, template <typename> typename Impl>
class PooledCollectionPtr {
  NameCollectionPool& pool_;
  T* collection_ = nullptr;

 protected:
  ~PooledCollectionPtr() { Impl<T>::releaseCollection(pool_, &collection_); }

  T& collection() {
    MOZ_ASSERT(collection_);
    return *collection_;
  }

  const T& collection() const {
    MOZ_ASSERT(collection_);
    return *collection_;
  }

 public:
  explicit PooledCollectionPtr(NameCollectionPool& pool) : pool_(pool) {}

  bool acquire(FrontendContext* fc) {
    MOZ_ASSERT(!collection_);
    collection_ = Impl<T>::acquireCollection(fc, pool_);
    return !!collection_;
  }

  explicit operator bool() const { return !!collection_; }

  T* operator->() { return &collection(); }

  const T* operator->() const { return &collection(); }

  T& operator*() { return collection(); }

  const T& operator*() const { return collection(); }
};

template <typename Map>
class PooledMapPtr : public PooledCollectionPtr<Map, PooledMapPtr> {
  friend class PooledCollectionPtr<Map, PooledMapPtr>;

  static Map* acquireCollection(FrontendContext* fc, NameCollectionPool& pool) {
    return pool.acquireMap<Map>(fc);
  }

  static void releaseCollection(NameCollectionPool& pool, Map** ptr) {
    pool.releaseMap(ptr);
  }

  using Base = PooledCollectionPtr<Map, PooledMapPtr>;

 public:
  using Base::Base;

  ~PooledMapPtr() = default;
};

template <typename Vector>
class PooledVectorPtr : public PooledCollectionPtr<Vector, PooledVectorPtr> {
  friend class PooledCollectionPtr<Vector, PooledVectorPtr>;

  static Vector* acquireCollection(FrontendContext* fc,
                                   NameCollectionPool& pool) {
    return pool.acquireVector<Vector>(fc);
  }

  static void releaseCollection(NameCollectionPool& pool, Vector** ptr) {
    pool.releaseVector(ptr);
  }

  using Base = PooledCollectionPtr<Vector, PooledVectorPtr>;
  using Base::collection;

 public:
  using Base::Base;

  ~PooledVectorPtr() = default;

  typename Vector::ElementType& operator[](size_t index) {
    return collection()[index];
  }

  const typename Vector::ElementType& operator[](size_t index) const {
    return collection()[index];
  }
};

}  // namespace js::frontend

#endif  // frontend_NameCollections_h
