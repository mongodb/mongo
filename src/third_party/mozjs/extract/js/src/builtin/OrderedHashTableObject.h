/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_OrderedHashTableObject_h
#define builtin_OrderedHashTableObject_h

/*
 * This file defines js::OrderedHashMapObject (a base class of js::MapObject)
 * and js::OrderedHashSetObject (a base class of js::SetObject).
 *
 * It also defines two templates, js::OrderedHashMapImpl and
 * js::OrderedHashSetImpl, that operate on these objects and implement the
 * ordered hash table algorithm. These templates are defined separately from
 * the JS object types because it lets us switch between different template
 * instantiations to enable or disable GC barriers.
 *
 * The implemented hash table algorithm is also different from HashMap and
 * HashSet:
 *
 *   - Iterating over an Ordered hash table visits the entries in the order in
 *     which they were inserted. This means that unlike a HashMap, the behavior
 *     of an OrderedHashMapImpl is deterministic (as long as the HashPolicy
 *     methods are effect-free and consistent); the hashing is a pure
 *     performance optimization.
 *
 *   - Iterator objects remain valid even when entries are added or removed or
 *     the table is resized.
 *
 * Hash policies
 *
 * See the comment about "Hash policy" in HashTable.h for general features that
 * hash policy classes must provide. Hash policies for OrderedHashMapImpl and
 * Sets differ in that the hash() method takes an extra argument:
 *
 *     static js::HashNumber hash(Lookup, const HashCodeScrambler&);
 *
 * They must additionally provide a distinguished "empty" key value and the
 * following static member functions:
 *
 *     bool isEmpty(const Key&);
 *     void makeEmpty(Key*);
 */

#include "mozilla/CheckedInt.h"
#include "mozilla/HashFunctions.h"
#include "mozilla/Likely.h"
#include "mozilla/Maybe.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/TemplateLib.h"

#include <memory>
#include <tuple>
#include <utility>

#include "builtin/SelfHostingDefines.h"
#include "gc/Barrier.h"
#include "gc/Zone.h"
#include "js/GCPolicyAPI.h"
#include "js/HashTable.h"
#include "vm/JSContext.h"
#include "vm/Realm.h"

class JSTracer;

namespace js {

class MapIteratorObject;
class SetIteratorObject;

namespace detail {

template <class T, class Ops>
class OrderedHashTableImpl;

// Base class for OrderedHashMapObject and OrderedHashSetObject.
class OrderedHashTableObject : public NativeObject {
  // Use a friend class to avoid exposing these slot definitions directly to
  // MapObject and SetObject.
  template <class T, class Ops>
  friend class OrderedHashTableImpl;

  enum Slots {
    HashTableSlot,
    DataSlot,
    DataLengthSlot,
    DataCapacitySlot,
    LiveCountSlot,
    HashShiftSlot,
    TenuredIteratorsSlot,
    NurseryIteratorsSlot,
    HashCodeScramblerSlot,
    SlotCount
  };

  inline void* allocateCellBuffer(JSContext* cx, size_t numBytes);

 public:
  static constexpr size_t offsetOfDataLength() {
    return getFixedSlotOffset(DataLengthSlot);
  }
  static constexpr size_t offsetOfData() {
    return getFixedSlotOffset(DataSlot);
  }
  static constexpr size_t offsetOfHashTable() {
    return getFixedSlotOffset(HashTableSlot);
  }
  static constexpr size_t offsetOfHashShift() {
    return getFixedSlotOffset(HashShiftSlot);
  }
  static constexpr size_t offsetOfLiveCount() {
    return getFixedSlotOffset(LiveCountSlot);
  }
  static constexpr size_t offsetOfHashCodeScrambler() {
    return getFixedSlotOffset(HashCodeScramblerSlot);
  }
};

}  // namespace detail

// Base class for MapIteratorObject and SetIteratorObject.
//
// Iterators remain valid for the lifetime of the OrderedHashTableObject, even
// if entries are added or removed or if the table is resized. Hash table
// objects have a doubly linked list of all active iterator objects and this
// lets us update the iterators when needed.
//
// An "active" iterator object has a target object and must be part of its
// linked list of iterators. When the iterator is finished, the target slot is
// cleared and the iterator is removed from the linked list.
//
// Note: the iterator list stores pointers to iterators as PrivateValue instead
// of ObjectValue because we want these links to be weak pointers: an iterator
// must not keep all other iterators alive.
class TableIteratorObject : public NativeObject {
  template <class T, class Ops>
  friend class detail::OrderedHashTableImpl;

 public:
  enum Slots {
    TargetSlot,
    KindSlot,
    IndexSlot,
    CountSlot,
    PrevPtrSlot,
    NextSlot,
    SlotCount
  };
  enum class Kind { Keys, Values, Entries };

  // Slot numbers and Kind values must match constants used in self-hosted code.
  static_assert(TargetSlot == ITERATOR_SLOT_TARGET);
  static_assert(KindSlot == MAP_SET_ITERATOR_SLOT_ITEM_KIND);
  static_assert(int32_t(Kind::Keys) == ITEM_KIND_KEY);
  static_assert(int32_t(Kind::Values) == ITEM_KIND_VALUE);
  static_assert(int32_t(Kind::Entries) == ITEM_KIND_KEY_AND_VALUE);

 private:
  // The index of the current entry within the table's data array.
  uint32_t getIndex() const {
    return getReservedSlot(IndexSlot).toPrivateUint32();
  }
  void setIndex(uint32_t i) {
    MOZ_ASSERT(isActive());
    setReservedSlotPrivateUint32Unbarriered(IndexSlot, i);
  }

  // The number of nonempty entries in the data array to the left of |index|.
  // This is used when the table is resized or compacted.
  uint32_t getCount() const {
    return getReservedSlot(CountSlot).toPrivateUint32();
  }
  void setCount(uint32_t i) {
    MOZ_ASSERT(isActive());
    setReservedSlotPrivateUint32Unbarriered(CountSlot, i);
  }

  // Links in the doubly-linked list of active iterator objects on the
  // OrderedHashTableObject.
  //
  // The PrevPtr slot points to either the previous iterator's NextSlot or to
  // the table's TenuredIteratorsSlot or NurseryIteratorsSlot if this is the
  // first iterator in the list.
  //
  // The Next slot points to the next iterator object, or is nullptr if this is
  // the last iterator in the list.
  //
  // Invariant: *getPrevPtr() == this.
  TableIteratorObject** getPrevPtr() const {
    MOZ_ASSERT(isActive());
    Value v = getReservedSlot(PrevPtrSlot);
    return static_cast<TableIteratorObject**>(v.toPrivate());
  }
  void setPrevPtr(TableIteratorObject** p) {
    MOZ_ASSERT(isActive());
    setReservedSlotPrivateUnbarriered(PrevPtrSlot, p);
  }
  TableIteratorObject* getNext() const {
    MOZ_ASSERT(isActive());
    Value v = getReservedSlot(NextSlot);
    return static_cast<TableIteratorObject*>(v.toPrivate());
  }
  TableIteratorObject** addressOfNext() {
    MOZ_ASSERT(isActive());
    return addressOfFixedSlotPrivatePtr<TableIteratorObject>(NextSlot);
  }
  void setNext(TableIteratorObject* p) {
    MOZ_ASSERT(isActive());
    setReservedSlotPrivateUnbarriered(NextSlot, p);
  }

  void link(TableIteratorObject** listp) {
    MOZ_ASSERT(isActive());
    TableIteratorObject* next = *listp;
    setPrevPtr(listp);
    setNext(next);
    *listp = this;
    if (next) {
      next->setPrevPtr(this->addressOfNext());
    }
  }

  void init(detail::OrderedHashTableObject* target, Kind kind,
            TableIteratorObject** listp) {
    initReservedSlot(TargetSlot, ObjectValue(*target));
    setReservedSlotPrivateUint32Unbarriered(KindSlot, uint32_t(kind));
    setReservedSlotPrivateUint32Unbarriered(IndexSlot, 0);
    setReservedSlotPrivateUint32Unbarriered(CountSlot, 0);
    link(listp);
  }

  void assertActiveIteratorFor(JSObject* target) {
    MOZ_ASSERT(&getReservedSlot(TargetSlot).toObject() == target);
    MOZ_ASSERT(*getPrevPtr() == this);
    MOZ_ASSERT(getNext() != this);
  }

 protected:
  bool isActive() const { return getReservedSlot(TargetSlot).isObject(); }

  void finish() {
    MOZ_ASSERT(isActive());
    unlink();
    // Mark iterator inactive.
    setReservedSlot(TargetSlot, UndefinedValue());
  }
  void unlink() {
    MOZ_ASSERT(isActive());
    TableIteratorObject** prevp = getPrevPtr();
    TableIteratorObject* next = getNext();
    *prevp = next;
    if (next) {
      next->setPrevPtr(prevp);
    }
  }

  // Update list pointers after a compacting GC moved this iterator in memory.
  // Note: this isn't used for nursery iterators because in that case we have to
  // rebuild the list with clearNurseryIterators and relinkNurseryIterator.
  void updateListAfterMove(TableIteratorObject* old) {
    MOZ_ASSERT(!IsInsideNursery(old));
    MOZ_ASSERT(isActive());
    MOZ_ASSERT(old != this);

    TableIteratorObject** prevp = getPrevPtr();
    MOZ_ASSERT(*prevp == old);
    *prevp = this;

    if (TableIteratorObject* next = getNext()) {
      MOZ_ASSERT(next->getPrevPtr() == old->addressOfNext());
      next->setPrevPtr(this->addressOfNext());
    }
  }

  Kind kind() const {
    uint32_t i = getReservedSlot(KindSlot).toPrivateUint32();
    MOZ_ASSERT(i == uint32_t(Kind::Keys) || i == uint32_t(Kind::Values) ||
               i == uint32_t(Kind::Entries));
    return Kind(i);
  }

 public:
  static constexpr size_t offsetOfTarget() {
    return getFixedSlotOffset(TargetSlot);
  }
  static constexpr size_t offsetOfIndex() {
    return getFixedSlotOffset(IndexSlot);
  }
  static constexpr size_t offsetOfCount() {
    return getFixedSlotOffset(CountSlot);
  }
  static constexpr size_t offsetOfPrevPtr() {
    return getFixedSlotOffset(PrevPtrSlot);
  }
  static constexpr size_t offsetOfNext() {
    return getFixedSlotOffset(NextSlot);
  }
};

namespace detail {

/*
 * detail::OrderedHashTableImpl is the underlying code used to implement both
 * OrderedHashMapImpl and OrderedHashSetImpl. Programs should use one of those
 * two templates rather than OrderedHashTableImpl.
 */
template <class T, class Ops>
class MOZ_STACK_CLASS OrderedHashTableImpl {
 public:
  using Key = typename Ops::KeyType;
  using MutableKey = std::remove_const_t<Key>;
  using UnbarrieredKey = typename RemoveBarrier<MutableKey>::Type;
  using Lookup = typename Ops::Lookup;
  using HashCodeScrambler = mozilla::HashCodeScrambler;
  static constexpr size_t SlotCount = OrderedHashTableObject::SlotCount;

  struct Data {
    T element;
    Data* chain;

    Data(const T& e, Data* c) : element(e), chain(c) {}
    Data(T&& e, Data* c) : element(std::move(e)), chain(c) {}
  };

 private:
  using Slots = OrderedHashTableObject::Slots;
  OrderedHashTableObject* const obj;

  // Whether we have allocated a buffer for this object. This buffer is
  // allocated when adding the first entry and it contains the data array, the
  // hash table buckets and the hash code scrambler.
  bool hasAllocatedBuffer() const {
    MOZ_ASSERT(hasInitializedSlots());
    return obj->getReservedSlot(Slots::DataSlot).toPrivate() != nullptr;
  }

  // Hash table. Has hashBuckets() elements.
  // Note: a single malloc buffer is used for the data and hashTable arrays and
  // the HashCodeScrambler. The pointer in DataSlot points to the start of this
  // buffer.
  Data** getHashTable() const {
    MOZ_ASSERT(hasAllocatedBuffer());
    Value v = obj->getReservedSlot(Slots::HashTableSlot);
    return static_cast<Data**>(v.toPrivate());
  }
  void setHashTable(Data** table) {
    obj->setReservedSlotPrivateUnbarriered(Slots::HashTableSlot, table);
  }

  // Array of Data objects. Elements data[0:dataLength] are constructed and the
  // total capacity is dataCapacity.
  //
  // maybeData returns nullptr if this object doesn't have a buffer yet.
  // getData asserts the object has a buffer.
  Data* maybeData() const {
    Value v = obj->getReservedSlot(Slots::DataSlot);
    return static_cast<Data*>(v.toPrivate());
  }
  Data* getData() const {
    MOZ_ASSERT(hasAllocatedBuffer());
    return maybeData();
  }
  void setData(Data* data) {
    obj->setReservedSlotPrivateUnbarriered(Slots::DataSlot, data);
  }

  // Number of constructed elements in the data array.
  uint32_t getDataLength() const {
    return obj->getReservedSlot(Slots::DataLengthSlot).toPrivateUint32();
  }
  void setDataLength(uint32_t length) {
    obj->setReservedSlotPrivateUint32Unbarriered(Slots::DataLengthSlot, length);
  }

  // Size of data array, in elements.
  uint32_t getDataCapacity() const {
    return obj->getReservedSlot(Slots::DataCapacitySlot).toPrivateUint32();
  }
  void setDataCapacity(uint32_t capacity) {
    obj->setReservedSlotPrivateUint32Unbarriered(Slots::DataCapacitySlot,
                                                 capacity);
  }

  // The number of elements in this table. This is different from dataLength
  // because the data array can contain empty/removed elements.
  uint32_t getLiveCount() const {
    return obj->getReservedSlot(Slots::LiveCountSlot).toPrivateUint32();
  }
  void setLiveCount(uint32_t liveCount) {
    obj->setReservedSlotPrivateUint32Unbarriered(Slots::LiveCountSlot,
                                                 liveCount);
  }

  // Multiplicative hash shift.
  uint32_t getHashShift() const {
    MOZ_ASSERT(hasAllocatedBuffer(),
               "hash shift is meaningless if there's no hash table");
    return obj->getReservedSlot(Slots::HashShiftSlot).toPrivateUint32();
  }
  void setHashShift(uint32_t hashShift) {
    obj->setReservedSlotPrivateUint32Unbarriered(Slots::HashShiftSlot,
                                                 hashShift);
  }

  // List of all active iterators on this table in the tenured heap. Populated
  // when iterators are created.
  TableIteratorObject* getTenuredIterators() const {
    Value v = obj->getReservedSlot(Slots::TenuredIteratorsSlot);
    return static_cast<TableIteratorObject*>(v.toPrivate());
  }
  void setTenuredIterators(TableIteratorObject* iter) {
    obj->setReservedSlotPrivateUnbarriered(Slots::TenuredIteratorsSlot, iter);
  }
  TableIteratorObject** addressOfTenuredIterators() const {
    static constexpr size_t slot = Slots::TenuredIteratorsSlot;
    return obj->addressOfFixedSlotPrivatePtr<TableIteratorObject>(slot);
  }

  // List of all active iterators on this table in the GC nursery.
  // Populated when iterators are created. This is cleared at the start
  // of minor GC and rebuilt when iterators are moved.
  TableIteratorObject* getNurseryIterators() const {
    Value v = obj->getReservedSlot(Slots::NurseryIteratorsSlot);
    return static_cast<TableIteratorObject*>(v.toPrivate());
  }
  void setNurseryIterators(TableIteratorObject* iter) {
    obj->setReservedSlotPrivateUnbarriered(Slots::NurseryIteratorsSlot, iter);
  }
  TableIteratorObject** addressOfNurseryIterators() const {
    static constexpr size_t slot = Slots::NurseryIteratorsSlot;
    return obj->addressOfFixedSlotPrivatePtr<TableIteratorObject>(slot);
  }

  // Returns a pointer to the list of tenured or nursery iterators depending on
  // where |iter| is allocated.
  TableIteratorObject** addressOfIterators(TableIteratorObject* iter) {
    return IsInsideNursery(iter) ? addressOfNurseryIterators()
                                 : addressOfTenuredIterators();
  }

  // Scrambler to not reveal pointer hash codes.
  const HashCodeScrambler* getHashCodeScrambler() const {
    MOZ_ASSERT(hasAllocatedBuffer());
    Value v = obj->getReservedSlot(Slots::HashCodeScramblerSlot);
    return static_cast<const HashCodeScrambler*>(v.toPrivate());
  }
  void setHashCodeScrambler(HashCodeScrambler* hcs) {
    obj->setReservedSlotPrivateUnbarriered(Slots::HashCodeScramblerSlot, hcs);
  }

  static constexpr uint32_t hashShiftToNumHashBuckets(uint32_t hashShift) {
    return 1 << (js::kHashNumberBits - hashShift);
  }
  static constexpr uint32_t numHashBucketsToDataCapacity(
      uint32_t numHashBuckets) {
    return uint32_t(numHashBuckets * FillFactor);
  }

  // Logarithm base 2 of the number of buckets in the hash table initially.
  static constexpr uint32_t InitialBucketsLog2 = 1;
  static constexpr uint32_t InitialBuckets = 1 << InitialBucketsLog2;
  static constexpr uint32_t InitialHashShift =
      js::kHashNumberBits - InitialBucketsLog2;

  // The maximum load factor (mean number of entries per bucket).
  // It is an invariant that
  //     dataCapacity == floor(hashBuckets * FillFactor).
  //
  // The fill factor should be between 2 and 4, and it should be chosen so that
  // the fill factor times sizeof(Data) is close to but <= a power of 2.
  // This fixed fill factor was chosen to make the size of the data
  // array, in bytes, close to a power of two when sizeof(T) is 16.
  static constexpr double FillFactor = 8.0 / 3.0;

  // The minimum permitted value of (liveCount / dataLength).
  // If that ratio drops below this value, we shrink the table.
  static constexpr double MinDataFill = 0.25;

  // Note: a lower hash shift results in a higher capacity.
  static constexpr uint32_t MinHashShift = 8;
  static constexpr uint32_t MaxHashBuckets =
      hashShiftToNumHashBuckets(MinHashShift);  // 16777216
  static constexpr uint32_t MaxDataCapacity =
      numHashBucketsToDataCapacity(MaxHashBuckets);  // 44739242

  template <typename F>
  void forEachIterator(F&& f) {
    TableIteratorObject* next;
    for (TableIteratorObject* iter = getTenuredIterators(); iter; iter = next) {
      next = iter->getNext();
      f(iter);
    }
    for (TableIteratorObject* iter = getNurseryIterators(); iter; iter = next) {
      next = iter->getNext();
      f(iter);
    }
  }

  bool hasInitializedSlots() const {
    return !obj->getReservedSlot(Slots::DataSlot).isUndefined();
  }

  static MOZ_ALWAYS_INLINE bool calcAllocSize(uint32_t dataCapacity,
                                              uint32_t buckets,
                                              size_t* numBytes) {
    using CheckedSize = mozilla::CheckedInt<size_t>;
    auto res = CheckedSize(dataCapacity) * sizeof(Data) +
               CheckedSize(sizeof(HashCodeScrambler)) +
               CheckedSize(buckets) * sizeof(Data*);
    if (MOZ_UNLIKELY(!res.isValid())) {
      return false;
    }
    *numBytes = res.value();
    return true;
  }

  // Allocate a single buffer that stores the data array followed by the hash
  // code scrambler and the hash table entries.
  using AllocationResult =
      std::tuple<Data*, Data**, HashCodeScrambler*, size_t>;
  AllocationResult allocateBuffer(JSContext* cx, uint32_t dataCapacity,
                                  uint32_t buckets) {
    MOZ_ASSERT(dataCapacity <= MaxDataCapacity);
    MOZ_ASSERT(buckets <= MaxHashBuckets);

    size_t numBytes = 0;
    if (MOZ_UNLIKELY(!calcAllocSize(dataCapacity, buckets, &numBytes))) {
      ReportAllocationOverflow(cx);
      return {};
    }

    void* buf = obj->allocateCellBuffer(cx, numBytes);
    if (!buf) {
      return {};
    }

    return getBufferParts(buf, numBytes, dataCapacity, buckets);
  }

  static AllocationResult getBufferParts(void* buf, size_t numBytes,
                                         uint32_t dataCapacity,
                                         uint32_t buckets) {
    static_assert(alignof(Data) % alignof(HashCodeScrambler) == 0,
                  "Hash code scrambler must be aligned properly");
    static_assert(alignof(HashCodeScrambler) % alignof(Data*) == 0,
                  "Hash table entries must be aligned properly");

    auto* data = static_cast<Data*>(buf);
    auto* hcs = reinterpret_cast<HashCodeScrambler*>(data + dataCapacity);
    auto** table = reinterpret_cast<Data**>(hcs + 1);

    MOZ_ASSERT(uintptr_t(table + buckets) == uintptr_t(buf) + numBytes);

    return {data, table, hcs, numBytes};
  }

  [[nodiscard]] bool initBuffer(JSContext* cx) {
    MOZ_ASSERT(!hasAllocatedBuffer());
    MOZ_ASSERT(getDataLength() == 0);
    MOZ_ASSERT(getLiveCount() == 0);

    constexpr uint32_t buckets = InitialBuckets;
    constexpr uint32_t capacity = uint32_t(buckets * FillFactor);

    auto [dataAlloc, tableAlloc, hcsAlloc, numBytes] =
        allocateBuffer(cx, capacity, buckets);
    if (!dataAlloc) {
      return false;
    }

    AddCellMemory(obj, numBytes, MemoryUse::MapObjectData);

    *hcsAlloc = cx->realm()->randomHashCodeScrambler();

    std::uninitialized_fill_n(tableAlloc, buckets, nullptr);

    setHashTable(tableAlloc);
    setData(dataAlloc);
    setDataCapacity(capacity);
    setHashShift(InitialHashShift);
    setHashCodeScrambler(hcsAlloc);
    MOZ_ASSERT(hashBuckets() == buckets);
    return true;
  }

  void updateHashTableForRekey(Data* entry, HashNumber oldHash,
                               HashNumber newHash) {
    uint32_t hashShift = getHashShift();
    oldHash >>= hashShift;
    newHash >>= hashShift;

    if (oldHash == newHash) {
      return;
    }

    // Remove this entry from its old hash chain. (If this crashes reading
    // nullptr, it would mean we did not find this entry on the hash chain where
    // we expected it. That probably means the key's hash code changed since it
    // was inserted, breaking the hash code invariant.)
    Data** hashTable = getHashTable();
    Data** ep = &hashTable[oldHash];
    while (*ep != entry) {
      ep = &(*ep)->chain;
    }
    *ep = entry->chain;

    // Add it to the new hash chain. We could just insert it at the beginning of
    // the chain. Instead, we do a bit of work to preserve the invariant that
    // hash chains always go in reverse insertion order (descending memory
    // order). No code currently depends on this invariant, so it's fine to kill
    // it if needed.
    ep = &hashTable[newHash];
    while (*ep && *ep > entry) {
      ep = &(*ep)->chain;
    }
    entry->chain = *ep;
    *ep = entry;
  }

 public:
  explicit OrderedHashTableImpl(OrderedHashTableObject* obj) : obj(obj) {}

  void initSlots() {
    MOZ_ASSERT(!hasInitializedSlots(), "init must be called at most once");
    setHashTable(nullptr);
    setData(nullptr);
    setDataLength(0);
    setDataCapacity(0);
    setLiveCount(0);
    setHashShift(0);
    setTenuredIterators(nullptr);
    setNurseryIterators(nullptr);
    setHashCodeScrambler(nullptr);
  }

  void destroy(JS::GCContext* gcx) {
    if (!hasInitializedSlots()) {
      return;
    }
    if (Data* data = maybeData()) {
      freeData(gcx, data, getDataLength(), getDataCapacity(), hashBuckets());
    }
  }

  void maybeMoveBufferOnPromotion(Nursery& nursery) {
    if (!hasAllocatedBuffer()) {
      return;
    }

    Data* oldData = getData();
    uint32_t dataCapacity = getDataCapacity();
    uint32_t buckets = hashBuckets();

    size_t numBytes = 0;
    MOZ_ALWAYS_TRUE(calcAllocSize(dataCapacity, buckets, &numBytes));

    void* buf = oldData;
    Nursery::WasBufferMoved result =
        nursery.maybeMoveNurseryOrMallocBufferOnPromotion(
            &buf, obj, numBytes, MemoryUse::MapObjectData);
    if (result == Nursery::BufferNotMoved) {
      return;
    }

    // The buffer was moved in memory. Update reserved slots and fix up the
    // |Data*| pointers for the hash table chains.
    // TODO(bug 1931492): consider storing indices instead of pointers to
    // simplify this.

    auto [data, table, hcs, numBytesUnused] =
        getBufferParts(buf, numBytes, dataCapacity, buckets);

    auto entryIndex = [=](const Data* entry) {
      MOZ_ASSERT(entry >= oldData);
      MOZ_ASSERT(size_t(entry - oldData) < dataCapacity);
      return entry - oldData;
    };

    for (uint32_t i = 0, len = getDataLength(); i < len; i++) {
      if (const Data* chain = data[i].chain) {
        data[i].chain = data + entryIndex(chain);
      }
    }
    for (uint32_t i = 0; i < buckets; i++) {
      if (const Data* chain = table[i]) {
        table[i] = data + entryIndex(chain);
      }
    }

    setData(data);
    setHashTable(table);
    setHashCodeScrambler(hcs);
  }

  size_t sizeOfExcludingObject(mozilla::MallocSizeOf mallocSizeOf) const {
    size_t size = 0;
    if (hasInitializedSlots() && hasAllocatedBuffer()) {
      // Note: this also includes the HashCodeScrambler and the hashTable array.
      size += mallocSizeOf(getData());
    }
    return size;
  }

  /* Return the number of elements in the table. */
  uint32_t count() const { return getLiveCount(); }

  /* True if any element matches l. */
  bool has(const Lookup& l) const { return lookup(l) != nullptr; }

  /* Return a pointer to the element, if any, that matches l, or nullptr. */
  T* get(const Lookup& l) {
    Data* e = lookup(l);
    return e ? &e->element : nullptr;
  }

  /*
   * If the table already contains an entry that matches |element|,
   * replace that entry with |element|. Otherwise add a new entry.
   *
   * On success, return true, whether there was already a matching element or
   * not. On allocation failure, return false. If this returns false, it
   * means the element was not added to the table.
   */
  template <typename ElementInput>
  [[nodiscard]] bool put(JSContext* cx, ElementInput&& element) {
    HashNumber h;
    if (hasAllocatedBuffer()) {
      h = prepareHash(Ops::getKey(element));
      if (Data* e = lookup(Ops::getKey(element), h)) {
        e->element = std::forward<ElementInput>(element);
        return true;
      }
      if (getDataLength() == getDataCapacity() && !rehashOnFull(cx)) {
        return false;
      }
    } else {
      if (!initBuffer(cx)) {
        return false;
      }
      h = prepareHash(Ops::getKey(element));
    }
    auto [entry, chain] = addEntry(h);
    new (entry) Data(std::forward<ElementInput>(element), chain);
    return true;
  }

#ifdef NIGHTLY_BUILD
  /*
   * If the table already contains an entry that matches |element|,
   * return that entry. Otherwise add a new entry.
   *
   * On success, return a pointer to the element, whether there was already a
   * matching element or not. On allocation failure, return a nullptr. If this
   * returns a nullptr, it means the element was not added to the table.
   */
  template <typename ElementInput>
  [[nodiscard]] T* getOrAdd(JSContext* cx, ElementInput&& element) {
    HashNumber h;
    if (hasAllocatedBuffer()) {
      h = prepareHash(Ops::getKey(element));
      if (Data* e = lookup(Ops::getKey(element), h)) {
        return &e->element;
      }
      if (getDataLength() == getDataCapacity() && !rehashOnFull(cx)) {
        return nullptr;
      }
    } else {
      if (!initBuffer(cx)) {
        return nullptr;
      }
      h = prepareHash(Ops::getKey(element));
    }
    auto [entry, chain] = addEntry(h);
    new (entry) Data(std::forward<ElementInput>(element), chain);
    return &entry->element;
  }
#endif  // #ifdef NIGHTLY_BUILD

  /*
   * If the table contains an element matching l, remove it and return true.
   * Otherwise return false.
   */
  bool remove(JSContext* cx, const Lookup& l) {
    // Note: This could be optimized so that removing the last entry,
    // data[dataLength - 1], decrements dataLength. LIFO use cases would
    // benefit.

    // If a matching entry exists, empty it.
    Data* e = lookup(l);
    if (e == nullptr) {
      return false;
    }

    MOZ_ASSERT(uint32_t(e - getData()) < getDataCapacity());

    uint32_t liveCount = getLiveCount();
    liveCount--;
    setLiveCount(liveCount);
    Ops::makeEmpty(&e->element);

    // Update active iterators.
    uint32_t pos = e - getData();
    forEachIterator(
        [this, pos](auto* iter) { IterOps::onRemove(obj, iter, pos); });

    // If many entries have been removed, try to shrink the table. Ignore OOM
    // because shrinking the table is an optimization and it's okay for it to
    // fail.
    if (hashBuckets() > InitialBuckets &&
        liveCount < getDataLength() * MinDataFill) {
      if (!rehash(cx, getHashShift() + 1)) {
        cx->recoverFromOutOfMemory();
      }
    }

    return true;
  }

  /*
   * Remove all entries.
   *
   * The effect on active iterators is the same as removing all entries; in
   * particular, those iterators are still active and will see any entries
   * added after a clear().
   */
  void clear(JSContext* cx) {
    if (getDataLength() != 0) {
      destroyData(getData(), getDataLength());
      setDataLength(0);
      setLiveCount(0);

      size_t buckets = hashBuckets();
      std::fill_n(getHashTable(), buckets, nullptr);

      forEachIterator([](auto* iter) { IterOps::onClear(iter); });

      // Try to shrink the table. Ignore OOM because shrinking the table is an
      // optimization and it's okay for it to fail.
      if (buckets > InitialBuckets) {
        if (!rehash(cx, InitialHashShift)) {
          cx->recoverFromOutOfMemory();
        }
      }
    }

    MOZ_ASSERT(getDataLength() == 0);
    MOZ_ASSERT(getLiveCount() == 0);
  }

  class IterOps {
    friend class OrderedHashTableImpl;

    static void init(OrderedHashTableObject* table, TableIteratorObject* iter,
                     TableIteratorObject::Kind kind) {
      auto** listp = OrderedHashTableImpl(table).addressOfIterators(iter);
      iter->init(table, kind, listp);
      seek(table, iter);
    }

    static void seek(OrderedHashTableObject* table, TableIteratorObject* iter) {
      iter->assertActiveIteratorFor(table);
      const Data* data = OrderedHashTableImpl(table).maybeData();
      uint32_t dataLength = OrderedHashTableImpl(table).getDataLength();
      uint32_t i = iter->getIndex();
      while (i < dataLength && Ops::isEmpty(Ops::getKey(data[i].element))) {
        i++;
      }
      iter->setIndex(i);
    }

    // The hash table calls this when an entry is removed.
    // j is the index of the removed entry.
    static void onRemove(OrderedHashTableObject* table,
                         TableIteratorObject* iter, uint32_t j) {
      iter->assertActiveIteratorFor(table);
      uint32_t i = iter->getIndex();
      if (j < i) {
        iter->setCount(iter->getCount() - 1);
      }
      if (j == i) {
        seek(table, iter);
      }
    }

    // The hash table calls this when the table is resized or compacted.
    // Since |count| is the number of nonempty entries to the left of |index|,
    // discarding the empty entries will not affect |count|, and it will make
    // |index| and |count| equal.
    static void onCompact(TableIteratorObject* iter) {
      iter->setIndex(iter->getCount());
    }

    // The hash table calls this when cleared.
    static void onClear(TableIteratorObject* iter) {
      iter->setIndex(0);
      iter->setCount(0);
    }

    // If the iterator reached the end of the data array, we're done: mark the
    // iterator inactive, remove it from the linked list, and return |true|.
    // Else, call |f| for the current entry, advance the iterator to the next
    // entry, and return |false|.
    template <typename F>
    static bool next(OrderedHashTableObject* obj, TableIteratorObject* iter,
                     F&& f) {
      iter->assertActiveIteratorFor(obj);

      OrderedHashTableImpl table(obj);
      uint32_t index = iter->getIndex();

      if (index >= table.getDataLength()) {
        iter->finish();
        return true;
      }

      f(iter->kind(), table.getData()[index].element);

      iter->setCount(iter->getCount() + 1);
      iter->setIndex(index + 1);
      seek(obj, iter);
      return false;
    }
  };

  // Calls |f| for each entry in the table. This function must not mutate the
  // table.
  template <typename F>
  [[nodiscard]] bool forEachEntry(F&& f) const {
    const Data* data = maybeData();
    uint32_t dataLength = getDataLength();
#ifdef DEBUG
    uint32_t liveCount = getLiveCount();
#endif
    for (uint32_t i = 0; i < dataLength; i++) {
      if (!Ops::isEmpty(Ops::getKey(data[i].element))) {
        if (!f(data[i].element)) {
          return false;
        }
      }
    }
    MOZ_ASSERT(maybeData() == data);
    MOZ_ASSERT(getDataLength() == dataLength);
    MOZ_ASSERT(getLiveCount() == liveCount);
    return true;
  }
#ifdef DEBUG
  // Like forEachEntry, but infallible and the function is called at most
  // maxCount times. This is useful for debug assertions.
  template <typename F>
  void forEachEntryUpTo(size_t maxCount, F&& f) const {
    MOZ_ASSERT(maxCount > 0);
    const Data* data = maybeData();
    uint32_t dataLength = getDataLength();
    uint32_t liveCount = getLiveCount();
    size_t count = 0;
    for (uint32_t i = 0; i < dataLength; i++) {
      if (!Ops::isEmpty(Ops::getKey(data[i].element))) {
        f(data[i].element);
        count++;
        if (count == maxCount) {
          break;
        }
      }
    }
    MOZ_ASSERT(maybeData() == data);
    MOZ_ASSERT(getDataLength() == dataLength);
    MOZ_ASSERT(getLiveCount() == liveCount);
  }
#endif

  void trace(JSTracer* trc) {
    Data* data = maybeData();
    uint32_t dataLength = getDataLength();
    for (uint32_t i = 0; i < dataLength; i++) {
      if (!Ops::isEmpty(Ops::getKey(data[i].element))) {
        Ops::trace(trc, this, i, data[i].element);
      }
    }
  }

  // For use by the implementation of Ops::trace.
  void traceKey(JSTracer* trc, uint32_t index, const Key& key) {
    MOZ_ASSERT(index < getDataLength());
    UnbarrieredKey newKey = key;
    JS::GCPolicy<UnbarrieredKey>::trace(trc, &newKey,
                                        "OrderedHashTableObject key");
    if (newKey != key) {
      rekey(&getData()[index], newKey);
    }
  }
  template <typename Value>
  void traceValue(JSTracer* trc, Value& value) {
    JS::GCPolicy<Value>::trace(trc, &value, "OrderedHashMapObject value");
  }

  void initIterator(TableIteratorObject* iter,
                    TableIteratorObject::Kind kind) const {
    IterOps::init(obj, iter, kind);
  }
  template <typename F>
  bool iteratorNext(TableIteratorObject* iter, F&& f) const {
    return IterOps::next(obj, iter, f);
  }

  void clearNurseryIterators() {
    if (TableIteratorObject* iter = getNurseryIterators()) {
      iter->setPrevPtr(nullptr);
    }
    setNurseryIterators(nullptr);
  }
  void relinkNurseryIterator(TableIteratorObject* iter) {
    auto** listp = addressOfIterators(iter);
    iter->link(listp);
  }

  void updateIteratorsAfterMove(OrderedHashTableObject* old) {
    if (TableIteratorObject* iter = getTenuredIterators()) {
      MOZ_ASSERT(iter->getPrevPtr() ==
                 OrderedHashTableImpl(old).addressOfTenuredIterators());
      iter->setPrevPtr(addressOfTenuredIterators());
    }
    if (TableIteratorObject* iter = getNurseryIterators()) {
      MOZ_ASSERT(iter->getPrevPtr() ==
                 OrderedHashTableImpl(old).addressOfNurseryIterators());
      iter->setPrevPtr(addressOfNurseryIterators());
    }
  }

  bool hasNurseryIterators() const { return getNurseryIterators(); }

  /*
   * Change the value of the given key.
   *
   * This calls Ops::hash on both the current key and the new key.
   * Ops::hash on the current key must return the same hash code as
   * when the entry was added to the table.
   */
  void rekeyOneEntry(const Key& current, const Key& newKey, const T& element) {
    if (current == newKey) {
      return;
    }

    HashNumber currentHash = prepareHash(current);
    HashNumber newHash = prepareHash(newKey);

    Data* entry = lookup(current, currentHash);
    MOZ_ASSERT(entry);
    entry->element = element;

    updateHashTableForRekey(entry, currentHash, newHash);
  }

  static constexpr size_t offsetOfDataElement() {
    static_assert(offsetof(Data, element) == 0,
                  "TableIteratorLoadEntry and TableIteratorAdvance depend on "
                  "offsetof(Data, element) being 0");
    return offsetof(Data, element);
  }
  static constexpr size_t offsetOfDataChain() { return offsetof(Data, chain); }
  static constexpr size_t sizeofData() { return sizeof(Data); }

#ifdef DEBUG
  mozilla::Maybe<HashNumber> hash(const Lookup& l) const {
    // We can only compute the hash number if we have an allocated buffer
    // because the buffer contains the hash code scrambler.
    if (!hasAllocatedBuffer()) {
      return {};
    }
    return mozilla::Some(prepareHash(l));
  }
#endif

 private:
  HashNumber prepareHash(const Lookup& l) const {
    MOZ_ASSERT(hasAllocatedBuffer(),
               "the hash code scrambler is allocated in the buffer");
    const HashCodeScrambler& hcs = *getHashCodeScrambler();
    return mozilla::ScrambleHashCode(Ops::hash(l, hcs));
  }

  /* The size of the hash table, in elements. Always a power of two. */
  uint32_t hashBuckets() const {
    return hashShiftToNumHashBuckets(getHashShift());
  }

  void destroyData(Data* data, uint32_t length) {
    Data* end = data + length;
    for (Data* p = data; p != end; p++) {
      p->~Data();
    }
  }

  void freeData(JS::GCContext* gcx, Data* data, uint32_t length,
                uint32_t capacity, uint32_t hashBuckets) {
    MOZ_ASSERT(data);
    MOZ_ASSERT(capacity > 0);

    destroyData(data, length);

    size_t numBytes;
    MOZ_ALWAYS_TRUE(calcAllocSize(capacity, hashBuckets, &numBytes));

    if (IsInsideNursery(obj)) {
      if (gcx->runtime()->gc.nursery().isInside(data)) {
        return;
      }
      gcx->runtime()->gc.nursery().removeMallocedBuffer(data, numBytes);
    }

    gcx->free_(obj, data, numBytes, MemoryUse::MapObjectData);
  }

  Data* lookup(const Lookup& l, HashNumber h) const {
    MOZ_ASSERT(hasAllocatedBuffer());
    Data** hashTable = getHashTable();
    uint32_t hashShift = getHashShift();
    for (Data* e = hashTable[h >> hashShift]; e; e = e->chain) {
      if (Ops::match(Ops::getKey(e->element), l)) {
        return e;
      }
    }
    return nullptr;
  }

  Data* lookup(const Lookup& l) const {
    // Note: checking |getLiveCount() > 0| is a minor performance optimization
    // but this check is also required for correctness because it implies
    // |hasAllocatedBuffer()|.
    if (getLiveCount() == 0) {
      return nullptr;
    }
    return lookup(l, prepareHash(l));
  }

  std::tuple<Data*, Data*> addEntry(HashNumber hash) {
    uint32_t dataLength = getDataLength();
    MOZ_ASSERT(dataLength < getDataCapacity());

    Data* entry = &getData()[dataLength];
    setDataLength(dataLength + 1);
    setLiveCount(getLiveCount() + 1);

    Data** hashTable = getHashTable();
    hash >>= getHashShift();
    Data* chain = hashTable[hash];
    hashTable[hash] = entry;

    return std::make_tuple(entry, chain);
  }

  /* This is called after rehashing the table. */
  void compacted() {
    // If we had any empty entries, compacting may have moved live entries
    // to the left within the data array. Notify all active iterators of
    // the change.
    forEachIterator([](auto* iter) { IterOps::onCompact(iter); });
  }

  /* Compact the entries in the data array and rehash them. */
  void rehashInPlace() {
    Data** hashTable = getHashTable();
    std::fill_n(hashTable, hashBuckets(), nullptr);

    Data* const data = getData();
    uint32_t hashShift = getHashShift();
    Data* wp = data;
    Data* end = data + getDataLength();
    for (Data* rp = data; rp != end; rp++) {
      if (!Ops::isEmpty(Ops::getKey(rp->element))) {
        HashNumber h = prepareHash(Ops::getKey(rp->element)) >> hashShift;
        if (rp != wp) {
          wp->element = std::move(rp->element);
        }
        wp->chain = hashTable[h];
        hashTable[h] = wp;
        wp++;
      }
    }
    MOZ_ASSERT(wp == data + getLiveCount());

    while (wp != end) {
      wp->~Data();
      wp++;
    }
    setDataLength(getLiveCount());
    compacted();
  }

  [[nodiscard]] bool rehashOnFull(JSContext* cx) {
    MOZ_ASSERT(getDataLength() == getDataCapacity());

    // If the hashTable is more than 1/4 deleted data, simply rehash in
    // place to free up some space. Otherwise, grow the table.
    uint32_t newHashShift = getLiveCount() >= getDataCapacity() * 0.75
                                ? getHashShift() - 1
                                : getHashShift();
    return rehash(cx, newHashShift);
  }

  /*
   * Grow, shrink, or compact both the hash table and data array.
   *
   * On success, this returns true, dataLength == liveCount, and there are no
   * empty elements in data[0:dataLength]. On allocation failure, this
   * leaves everything as it was and returns false.
   */
  [[nodiscard]] bool rehash(JSContext* cx, uint32_t newHashShift) {
    // If the size of the table is not changing, rehash in place to avoid
    // allocating memory.
    if (newHashShift == getHashShift()) {
      rehashInPlace();
      return true;
    }

    if (MOZ_UNLIKELY(newHashShift < MinHashShift)) {
      ReportAllocationOverflow(cx);
      return false;
    }

    uint32_t newHashBuckets = hashShiftToNumHashBuckets(newHashShift);
    uint32_t newCapacity = numHashBucketsToDataCapacity(newHashBuckets);

    auto [newData, newHashTable, newHcs, numBytes] =
        allocateBuffer(cx, newCapacity, newHashBuckets);
    if (!newData) {
      return false;
    }

    *newHcs = *getHashCodeScrambler();

    std::uninitialized_fill_n(newHashTable, newHashBuckets, nullptr);

    Data* const oldData = getData();
    const uint32_t oldDataLength = getDataLength();

    Data* wp = newData;
    Data* end = oldData + oldDataLength;
    for (Data* p = oldData; p != end; p++) {
      if (!Ops::isEmpty(Ops::getKey(p->element))) {
        HashNumber h = prepareHash(Ops::getKey(p->element)) >> newHashShift;
        new (wp) Data(std::move(p->element), newHashTable[h]);
        newHashTable[h] = wp;
        wp++;
      }
    }
    MOZ_ASSERT(wp == newData + getLiveCount());

    freeData(obj->runtimeFromMainThread()->gcContext(), oldData, oldDataLength,
             getDataCapacity(), hashBuckets());

    AddCellMemory(obj, numBytes, MemoryUse::MapObjectData);

    setHashTable(newHashTable);
    setData(newData);
    setDataLength(getLiveCount());
    setDataCapacity(newCapacity);
    setHashShift(newHashShift);
    setHashCodeScrambler(newHcs);
    MOZ_ASSERT(hashBuckets() == newHashBuckets);

    compacted();
    return true;
  }

  // Change the key of the front entry.
  //
  // This calls Ops::hash on both the current key and the new key. Ops::hash on
  // the current key must return the same hash code as when the entry was added
  // to the table.
  void rekey(Data* entry, const UnbarrieredKey& k) {
    HashNumber oldHash = prepareHash(Ops::getKey(entry->element));
    HashNumber newHash = prepareHash(k);
    reinterpret_cast<UnbarrieredKey&>(Ops::getKeyRef(entry->element)) = k;
    updateHashTableForRekey(entry, oldHash, newHash);
  }
};

}  // namespace detail

class OrderedHashMapObject : public detail::OrderedHashTableObject {};

template <class Key, class Value, class OrderedHashPolicy>
class MOZ_STACK_CLASS OrderedHashMapImpl {
 public:
  class Entry {
    template <class, class>
    friend class detail::OrderedHashTableImpl;
    void operator=(const Entry& rhs) {
      const_cast<Key&>(key) = rhs.key;
      value = rhs.value;
    }

    void operator=(Entry&& rhs) {
      MOZ_ASSERT(this != &rhs, "self-move assignment is prohibited");
      const_cast<Key&>(key) = std::move(rhs.key);
      value = std::move(rhs.value);
    }

   public:
    Entry() = default;
    explicit Entry(const Key& k) : key(k) {}
    template <typename V>
    Entry(const Key& k, V&& v) : key(k), value(std::forward<V>(v)) {}
    Entry(Entry&& rhs) : key(std::move(rhs.key)), value(std::move(rhs.value)) {}

    const Key key{};
    Value value{};

    static constexpr size_t offsetOfKey() { return offsetof(Entry, key); }
    static constexpr size_t offsetOfValue() { return offsetof(Entry, value); }
  };

 private:
  struct MapOps;
  using Impl = detail::OrderedHashTableImpl<Entry, MapOps>;

  struct MapOps : OrderedHashPolicy {
    using KeyType = Key;
    static void makeEmpty(Entry* e) {
      OrderedHashPolicy::makeEmpty(const_cast<Key*>(&e->key));

      // Clear the value. Destroying it is another possibility, but that
      // would complicate class Entry considerably.
      e->value = Value();
    }
    static const Key& getKey(const Entry& e) { return e.key; }
    static Key& getKeyRef(Entry& e) { return const_cast<Key&>(e.key); }
    static void trace(JSTracer* trc, Impl* table, uint32_t index,
                      Entry& entry) {
      table->traceKey(trc, index, entry.key);
      table->traceValue(trc, entry.value);
    }
  };

  Impl impl;

 public:
  using Lookup = typename Impl::Lookup;
  static constexpr size_t SlotCount = Impl::SlotCount;

  explicit OrderedHashMapImpl(OrderedHashMapObject* obj) : impl(obj) {}

  void initSlots() { impl.initSlots(); }
  uint32_t count() const { return impl.count(); }
  bool has(const Lookup& key) const { return impl.has(key); }
  template <typename F>
  [[nodiscard]] bool forEachEntry(F&& f) const {
    return impl.forEachEntry(f);
  }
#ifdef DEBUG
  template <typename F>
  void forEachEntryUpTo(size_t maxCount, F&& f) const {
    impl.forEachEntryUpTo(maxCount, f);
  }
#endif
  Entry* get(const Lookup& key) { return impl.get(key); }
  bool remove(JSContext* cx, const Lookup& key) { return impl.remove(cx, key); }
  void clear(JSContext* cx) { impl.clear(cx); }

  void destroy(JS::GCContext* gcx) { impl.destroy(gcx); }

  template <typename K, typename V>
  [[nodiscard]] bool put(JSContext* cx, K&& key, V&& value) {
    return impl.put(cx, Entry(std::forward<K>(key), std::forward<V>(value)));
  }

#ifdef NIGHTLY_BUILD
  template <typename K, typename V>
  [[nodiscard]] Entry* getOrAdd(JSContext* cx, K&& key, V&& value) {
    return impl.getOrAdd(cx,
                         Entry(std::forward<K>(key), std::forward<V>(value)));
  }
#endif  // #ifdef NIGHTLY_BUILD

#ifdef DEBUG
  mozilla::Maybe<HashNumber> hash(const Lookup& key) const {
    return impl.hash(key);
  }
#endif

  template <typename GetNewKey>
  mozilla::Maybe<Key> rekeyOneEntry(Lookup& current, GetNewKey&& getNewKey) {
    // TODO: This is inefficient because we also look up the entry in
    // impl.rekeyOneEntry below.
    const Entry* e = get(current);
    if (!e) {
      return mozilla::Nothing();
    }

    Key newKey = getNewKey(current);
    impl.rekeyOneEntry(current, newKey, Entry(newKey, e->value));
    return mozilla::Some(newKey);
  }

  void initIterator(MapIteratorObject* iter,
                    TableIteratorObject::Kind kind) const {
    impl.initIterator(iter, kind);
  }
  template <typename F>
  bool iteratorNext(MapIteratorObject* iter, F&& f) const {
    return impl.iteratorNext(iter, f);
  }

  void clearNurseryIterators() { impl.clearNurseryIterators(); }
  void relinkNurseryIterator(MapIteratorObject* iter) {
    impl.relinkNurseryIterator(iter);
  }
  void updateIteratorsAfterMove(OrderedHashMapObject* old) {
    impl.updateIteratorsAfterMove(old);
  }
  bool hasNurseryIterators() const { return impl.hasNurseryIterators(); }

  void maybeMoveBufferOnPromotion(Nursery& nursery) {
    return impl.maybeMoveBufferOnPromotion(nursery);
  }

  void trace(JSTracer* trc) { impl.trace(trc); }

  static constexpr size_t offsetOfEntryKey() { return Entry::offsetOfKey(); }
  static constexpr size_t offsetOfImplDataElement() {
    return Impl::offsetOfDataElement();
  }
  static constexpr size_t offsetOfImplDataChain() {
    return Impl::offsetOfDataChain();
  }
  static constexpr size_t sizeofImplData() { return Impl::sizeofData(); }

  size_t sizeOfExcludingObject(mozilla::MallocSizeOf mallocSizeOf) const {
    return impl.sizeOfExcludingObject(mallocSizeOf);
  }
};

class OrderedHashSetObject : public detail::OrderedHashTableObject {};

template <class T, class OrderedHashPolicy>
class MOZ_STACK_CLASS OrderedHashSetImpl {
 private:
  struct SetOps;
  using Impl = detail::OrderedHashTableImpl<T, SetOps>;

  struct SetOps : OrderedHashPolicy {
    using KeyType = const T;
    static const T& getKey(const T& v) { return v; }
    static T& getKeyRef(T& e) { return e; }
    static void trace(JSTracer* trc, Impl* table, uint32_t index, T& entry) {
      table->traceKey(trc, index, entry);
    }
  };

  Impl impl;

 public:
  using Lookup = typename Impl::Lookup;
  static constexpr size_t SlotCount = Impl::SlotCount;

  explicit OrderedHashSetImpl(OrderedHashSetObject* obj) : impl(obj) {}

  void initSlots() { impl.initSlots(); }
  uint32_t count() const { return impl.count(); }
  bool has(const Lookup& value) const { return impl.has(value); }
  template <typename F>
  [[nodiscard]] bool forEachEntry(F&& f) const {
    return impl.forEachEntry(f);
  }
#ifdef DEBUG
  template <typename F>
  void forEachEntryUpTo(size_t maxCount, F&& f) const {
    impl.forEachEntryUpTo(maxCount, f);
  }
#endif
  template <typename Input>
  [[nodiscard]] bool put(JSContext* cx, Input&& value) {
    return impl.put(cx, std::forward<Input>(value));
  }
  bool remove(JSContext* cx, const Lookup& value) {
    return impl.remove(cx, value);
  }
  void clear(JSContext* cx) { impl.clear(cx); }

  void destroy(JS::GCContext* gcx) { impl.destroy(gcx); }

#ifdef DEBUG
  mozilla::Maybe<HashNumber> hash(const Lookup& value) const {
    return impl.hash(value);
  }
#endif

  template <typename GetNewKey>
  mozilla::Maybe<T> rekeyOneEntry(Lookup& current, GetNewKey&& getNewKey) {
    // TODO: This is inefficient because we also look up the entry in
    // impl.rekeyOneEntry below.
    if (!has(current)) {
      return mozilla::Nothing();
    }

    T newKey = getNewKey(current);
    impl.rekeyOneEntry(current, newKey, newKey);
    return mozilla::Some(newKey);
  }

  void initIterator(SetIteratorObject* iter,
                    TableIteratorObject::Kind kind) const {
    impl.initIterator(iter, kind);
  }
  template <typename F>
  bool iteratorNext(SetIteratorObject* iter, F&& f) const {
    return impl.iteratorNext(iter, f);
  }

  void clearNurseryIterators() { impl.clearNurseryIterators(); }
  void relinkNurseryIterator(SetIteratorObject* iter) {
    impl.relinkNurseryIterator(iter);
  }
  void updateIteratorsAfterMove(OrderedHashSetObject* old) {
    impl.updateIteratorsAfterMove(old);
  }
  bool hasNurseryIterators() const { return impl.hasNurseryIterators(); }

  void maybeMoveBufferOnPromotion(Nursery& nursery) {
    return impl.maybeMoveBufferOnPromotion(nursery);
  }

  void trace(JSTracer* trc) { impl.trace(trc); }

  static constexpr size_t offsetOfEntryKey() { return 0; }
  static constexpr size_t offsetOfImplDataElement() {
    return Impl::offsetOfDataElement();
  }
  static constexpr size_t offsetOfImplDataChain() {
    return Impl::offsetOfDataChain();
  }
  static constexpr size_t sizeofImplData() { return Impl::sizeofData(); }

  size_t sizeOfExcludingObject(mozilla::MallocSizeOf mallocSizeOf) const {
    return impl.sizeOfExcludingObject(mallocSizeOf);
  }
};

}  // namespace js

#endif /* builtin_OrderedHashTableObject_h */
