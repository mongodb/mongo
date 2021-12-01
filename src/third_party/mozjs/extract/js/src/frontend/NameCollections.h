/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_NameCollections_h
#define frontend_NameCollections_h

#include "ds/InlineTable.h"
#include "frontend/NameAnalysisTypes.h"
#include "js/Vector.h"
#include "vm/Stack.h"

namespace js {
namespace frontend {

// A pool of recyclable containers for use in the frontend. The Parser and
// BytecodeEmitter create many maps for name analysis that are short-lived
// (i.e., for the duration of parsing or emitting a lexical scope). Making
// them recyclable cuts down significantly on allocator churn.
template <typename RepresentativeCollection, typename ConcreteCollectionPool>
class CollectionPool
{
    using RecyclableCollections = Vector<void*, 32, SystemAllocPolicy>;

    RecyclableCollections all_;
    RecyclableCollections recyclable_;

    static RepresentativeCollection* asRepresentative(void* p) {
        return reinterpret_cast<RepresentativeCollection*>(p);
    }

    RepresentativeCollection* allocate() {
        size_t newAllLength = all_.length() + 1;
        if (!all_.reserve(newAllLength) || !recyclable_.reserve(newAllLength))
            return nullptr;

        RepresentativeCollection* collection = js_new<RepresentativeCollection>();
        if (collection)
            all_.infallibleAppend(collection);
        return collection;
    }

  public:
    ~CollectionPool() {
        purgeAll();
    }

    void purgeAll() {
        void** end = all_.end();
        for (void** it = all_.begin(); it != end; ++it)
            js_delete(asRepresentative(*it));

        all_.clearAndFree();
        recyclable_.clearAndFree();
    }

    // Fallibly aquire one of the supported collection types from the pool.
    template <typename Collection>
    Collection* acquire(JSContext* cx) {
        ConcreteCollectionPool::template assertInvariants<Collection>();

        RepresentativeCollection* collection;
        if (recyclable_.empty()) {
            collection = allocate();
            if (!collection)
                ReportOutOfMemory(cx);
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
        for (void** it = recyclable_.begin(); it != recyclable_.end(); ++it)
            MOZ_ASSERT(*it != *collection);
#endif

        MOZ_ASSERT(recyclable_.length() < all_.length());
        // Reserved in allocateFresh.
        recyclable_.infallibleAppend(*collection);
        *collection = nullptr;
    }
};

template <typename Wrapped>
struct RecyclableAtomMapValueWrapper
{
    union {
        Wrapped wrapped;
        uint64_t dummy;
    };

    static void assertInvariant() {
        static_assert(sizeof(Wrapped) <= sizeof(uint64_t),
                      "Can only recycle atom maps with values smaller than uint64");
    }

    RecyclableAtomMapValueWrapper() {
        assertInvariant();
    }

    MOZ_IMPLICIT RecyclableAtomMapValueWrapper(Wrapped w)
      : wrapped(w)
    {
        assertInvariant();
    }

    MOZ_IMPLICIT operator Wrapped&() {
        return wrapped;
    }

    MOZ_IMPLICIT operator Wrapped&() const {
        return wrapped;
    }

    Wrapped* operator->() {
        return &wrapped;
    }

    const Wrapped* operator->() const {
        return &wrapped;
    }
};

template <typename MapValue>
using RecyclableNameMap = InlineMap<JSAtom*,
                                    RecyclableAtomMapValueWrapper<MapValue>,
                                    24,
                                    DefaultHasher<JSAtom*>,
                                    SystemAllocPolicy>;

using DeclaredNameMap = RecyclableNameMap<DeclaredNameInfo>;
using CheckTDZMap = RecyclableNameMap<MaybeCheckTDZ>;
using NameLocationMap = RecyclableNameMap<NameLocation>;
using AtomIndexMap = RecyclableNameMap<uint32_t>;

template <typename RepresentativeTable>
class InlineTablePool
  : public CollectionPool<RepresentativeTable, InlineTablePool<RepresentativeTable>>
{
  public:
    template <typename Table>
    static void assertInvariants() {
        static_assert(Table::SizeOfInlineEntries == RepresentativeTable::SizeOfInlineEntries,
                      "Only tables with the same size for inline entries are usable in the pool.");
        static_assert(mozilla::IsPod<typename Table::Table::Entry>::value,
                      "Only tables with POD values are usable in the pool.");
    }
};

using FunctionBoxVector = Vector<FunctionBox*, 24, SystemAllocPolicy>;

template <typename RepresentativeVector>
class VectorPool : public CollectionPool<RepresentativeVector, VectorPool<RepresentativeVector>>
{
  public:
    template <typename Vector>
    static void assertInvariants() {
        static_assert(Vector::sMaxInlineStorage == RepresentativeVector::sMaxInlineStorage,
                      "Only vectors with the same size for inline entries are usable in the pool.");
        static_assert(mozilla::IsPod<typename Vector::ElementType>::value,
                      "Only vectors of POD values are usable in the pool.");
        static_assert(sizeof(typename Vector::ElementType) ==
                      sizeof(typename RepresentativeVector::ElementType),
                      "Only vectors with same-sized elements are usable in the pool.");
    }
};

class NameCollectionPool
{
    InlineTablePool<AtomIndexMap> mapPool_;
    VectorPool<AtomVector> vectorPool_;
    uint32_t activeCompilations_;

  public:
    NameCollectionPool()
      : activeCompilations_(0)
    { }

    bool hasActiveCompilation() const {
        return activeCompilations_ != 0;
    }

    void addActiveCompilation() {
        activeCompilations_++;
    }

    void removeActiveCompilation() {
        MOZ_ASSERT(hasActiveCompilation());
        activeCompilations_--;
    }

    template <typename Map>
    Map* acquireMap(JSContext* cx) {
        MOZ_ASSERT(hasActiveCompilation());
        return mapPool_.acquire<Map>(cx);
    }

    template <typename Map>
    void releaseMap(Map** map) {
        MOZ_ASSERT(hasActiveCompilation());
        MOZ_ASSERT(map);
        if (*map)
            mapPool_.release(map);
    }

    template <typename Vector>
    Vector* acquireVector(JSContext* cx) {
        MOZ_ASSERT(hasActiveCompilation());
        return vectorPool_.acquire<Vector>(cx);
    }

    template <typename Vector>
    void releaseVector(Vector** vec) {
        MOZ_ASSERT(hasActiveCompilation());
        MOZ_ASSERT(vec);
        if (*vec)
            vectorPool_.release(vec);
    }

    void purge() {
        if (!hasActiveCompilation()) {
            mapPool_.purgeAll();
            vectorPool_.purgeAll();
        }
    }
};

#define POOLED_COLLECTION_PTR_METHODS(N, T)                       \
    NameCollectionPool& pool_;                                    \
    T* collection_;                                               \
                                                                  \
    T& collection() {                                             \
        MOZ_ASSERT(collection_);                                  \
        return *collection_;                                      \
    }                                                             \
                                                                  \
    const T& collection() const {                                 \
        MOZ_ASSERT(collection_);                                  \
        return *collection_;                                      \
    }                                                             \
                                                                  \
  public:                                                         \
    explicit N(NameCollectionPool& pool)                          \
      : pool_(pool),                                              \
        collection_(nullptr)                                      \
    { }                                                           \
                                                                  \
    ~N() {                                                        \
        pool_.release##T(&collection_);                           \
    }                                                             \
                                                                  \
    bool acquire(JSContext* cx) {                                 \
        MOZ_ASSERT(!collection_);                                 \
        collection_ = pool_.acquire##T<T>(cx);                    \
        return !!collection_;                                     \
    }                                                             \
                                                                  \
    explicit operator bool() const {                              \
        return !!collection_;                                     \
    }                                                             \
                                                                  \
    T* operator->() {                                             \
        return &collection();                                     \
    }                                                             \
                                                                  \
    const T* operator->() const {                                 \
        return &collection();                                     \
    }                                                             \
                                                                  \
    T& operator*() {                                              \
        return collection();                                      \
    }                                                             \
                                                                  \
    const T& operator*() const {                                  \
        return collection();                                      \
    }

template <typename Map>
class PooledMapPtr
{
    POOLED_COLLECTION_PTR_METHODS(PooledMapPtr, Map)
};

template <typename Vector>
class PooledVectorPtr
{
    POOLED_COLLECTION_PTR_METHODS(PooledVectorPtr, Vector)

    typename Vector::ElementType& operator[](size_t index) {
        return collection()[index];
    }

    const typename Vector::ElementType& operator[](size_t index) const {
        return collection()[index];
    }
};

#undef POOLED_COLLECTION_PTR_METHODS

} // namespace frontend
} // namespace js

namespace mozilla {

template <>
struct IsPod<js::MaybeCheckTDZ> : TrueType {};

template <typename T>
struct IsPod<js::frontend::RecyclableAtomMapValueWrapper<T>> : IsPod<T> {};

} // namespace mozilla

#endif // frontend_NameCollections_h
