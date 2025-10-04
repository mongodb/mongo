/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef ds_OrderedHashTable_h
#define ds_OrderedHashTable_h

/*
 * Define two collection templates, js::OrderedHashMap and js::OrderedHashSet.
 * They are like js::HashMap and js::HashSet except that:
 *
 *   - Iterating over an Ordered hash table visits the entries in the order in
 *     which they were inserted. This means that unlike a HashMap, the behavior
 *     of an OrderedHashMap is deterministic (as long as the HashPolicy methods
 *     are effect-free and consistent); the hashing is a pure performance
 *     optimization.
 *
 *   - Range objects over Ordered tables remain valid even when entries are
 *     added or removed or the table is resized. (However in the case of
 *     removing entries, note the warning on class Range below.)
 *
 *   - The API is a little different, so it's not a drop-in replacement.
 *     In particular, the hash policy is a little different.
 *     Also, the Ordered templates lack the Ptr and AddPtr types.
 *
 * Hash policies
 *
 * See the comment about "Hash policy" in HashTable.h for general features that
 * hash policy classes must provide. Hash policies for OrderedHashMaps and Sets
 * differ in that the hash() method takes an extra argument:
 *     static js::HashNumber hash(Lookup, const HashCodeScrambler&);
 * They must additionally provide a distinguished "empty" key value and the
 * following static member functions:
 *     bool isEmpty(const Key&);
 *     void makeEmpty(Key*);
 */

#include "mozilla/HashFunctions.h"
#include "mozilla/Likely.h"
#include "mozilla/Maybe.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/TemplateLib.h"

#include <tuple>
#include <utility>

#include "gc/Barrier.h"
#include "js/GCPolicyAPI.h"
#include "js/HashTable.h"

class JSTracer;

namespace js {

namespace detail {

/*
 * detail::OrderedHashTable is the underlying data structure used to implement
 * both OrderedHashMap and OrderedHashSet. Programs should use one of those two
 * templates rather than OrderedHashTable.
 */
template <class T, class Ops, class AllocPolicy>
class OrderedHashTable {
 public:
  using Key = typename Ops::KeyType;
  using Lookup = typename Ops::Lookup;

  struct Data {
    T element;
    Data* chain;

    Data(const T& e, Data* c) : element(e), chain(c) {}
    Data(T&& e, Data* c) : element(std::move(e)), chain(c) {}
  };

  class Range;
  friend class Range;

 private:
  Data** hashTable;       // hash table (has hashBuckets() elements)
  Data* data;             // data vector, an array of Data objects
                          // data[0:dataLength] are constructed
  uint32_t dataLength;    // number of constructed elements in data
  uint32_t dataCapacity;  // size of data, in elements
  uint32_t liveCount;     // dataLength less empty (removed) entries
  uint32_t hashShift;     // multiplicative hash shift

  // List of all live Ranges on this table in malloc memory. Populated when
  // ranges are created.
  Range* ranges;

  // List of all live Ranges on this table in the GC nursery. Populated when
  // ranges are created. This is cleared at the start of minor GC and rebuilt
  // when ranges are moved.
  Range* nurseryRanges;

  AllocPolicy alloc;
  mozilla::HashCodeScrambler hcs;  // don't reveal pointer hash codes

  // TODO: This should be templated on a functor type and receive lambda
  // arguments but this causes problems for the hazard analysis builds. See
  // bug 1398213.
  template <void (*f)(Range* range, uint32_t arg)>
  void forEachRange(uint32_t arg = 0) {
    Range* next;
    for (Range* r = ranges; r; r = next) {
      next = r->next;
      f(r, arg);
    }
    for (Range* r = nurseryRanges; r; r = next) {
      next = r->next;
      f(r, arg);
    }
  }

 public:
  OrderedHashTable(AllocPolicy ap, mozilla::HashCodeScrambler hcs)
      : hashTable(nullptr),
        data(nullptr),
        dataLength(0),
        dataCapacity(0),
        liveCount(0),
        hashShift(0),
        ranges(nullptr),
        nurseryRanges(nullptr),
        alloc(std::move(ap)),
        hcs(hcs) {}

  [[nodiscard]] bool init() {
    MOZ_ASSERT(!hashTable, "init must be called at most once");

    uint32_t buckets = initialBuckets();
    Data** tableAlloc = alloc.template pod_malloc<Data*>(buckets);
    if (!tableAlloc) {
      return false;
    }
    for (uint32_t i = 0; i < buckets; i++) {
      tableAlloc[i] = nullptr;
    }

    uint32_t capacity = uint32_t(buckets * fillFactor());
    Data* dataAlloc = alloc.template pod_malloc<Data>(capacity);
    if (!dataAlloc) {
      alloc.free_(tableAlloc, buckets);
      return false;
    }

    // clear() requires that members are assigned only after all allocation
    // has succeeded, and that this->ranges is left untouched.
    hashTable = tableAlloc;
    data = dataAlloc;
    dataLength = 0;
    dataCapacity = capacity;
    liveCount = 0;
    hashShift = js::kHashNumberBits - initialBucketsLog2();
    MOZ_ASSERT(hashBuckets() == buckets);
    return true;
  }

  ~OrderedHashTable() {
    forEachRange<Range::onTableDestroyed>();
    if (hashTable) {
      // |hashBuckets()| isn't valid when |hashTable| hasn't been created.
      alloc.free_(hashTable, hashBuckets());
    }
    freeData(data, dataLength, dataCapacity);
  }

  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
    size_t size = 0;
    if (hashTable) {
      size += mallocSizeOf(hashTable);
    }
    if (data) {
      size += mallocSizeOf(data);
    }
    return size;
  }

  /* Return the number of elements in the table. */
  uint32_t count() const { return liveCount; }

  /* True if any element matches l. */
  bool has(const Lookup& l) const { return lookup(l) != nullptr; }

  /* Return a pointer to the element, if any, that matches l, or nullptr. */
  T* get(const Lookup& l) {
    Data* e = lookup(l, prepareHash(l));
    return e ? &e->element : nullptr;
  }

  /* Return a pointer to the element, if any, that matches l, or nullptr. */
  const T* get(const Lookup& l) const {
    return const_cast<OrderedHashTable*>(this)->get(l);
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
  [[nodiscard]] bool put(ElementInput&& element) {
    HashNumber h = prepareHash(Ops::getKey(element));
    if (Data* e = lookup(Ops::getKey(element), h)) {
      e->element = std::forward<ElementInput>(element);
      return true;
    }

    if (dataLength == dataCapacity && !rehashOnFull()) {
      return false;
    }

    auto [entry, chain] = addEntry(h);
    new (entry) Data(std::forward<ElementInput>(element), chain);
    return true;
  }

  /*
   * If the table contains an entry that matches |element| then return a pointer
   * to it, otherwise add a new entry.
   */
  template <typename ElementInput>
  [[nodiscard]] T* getOrAdd(ElementInput&& element) {
    HashNumber h = prepareHash(Ops::getKey(element));
    if (Data* e = lookup(Ops::getKey(element), h)) {
      return &e->element;
    }

    if (dataLength == dataCapacity && !rehashOnFull()) {
      return nullptr;
    }

    auto [entry, chain] = addEntry(h);
    new (entry) Data(std::forward<ElementInput>(element), chain);
    return &entry->element;
  }

  /*
   * If the table contains an element matching l, remove it and set *foundp
   * to true. Otherwise set *foundp to false.
   *
   * Return true on success, false if we tried to shrink the table and hit an
   * allocation failure. Even if this returns false, *foundp is set correctly
   * and the matching element was removed. Shrinking is an optimization and
   * it's OK for it to fail.
   */
  bool remove(const Lookup& l, bool* foundp) {
    // Note: This could be optimized so that removing the last entry,
    // data[dataLength - 1], decrements dataLength. LIFO use cases would
    // benefit.

    // If a matching entry exists, empty it.
    Data* e = lookup(l, prepareHash(l));
    if (e == nullptr) {
      *foundp = false;
      return true;
    }

    *foundp = true;
    return remove(e);
  }

  bool remove(Data* e) {
    MOZ_ASSERT(uint32_t(e - data) < dataCapacity);

    liveCount--;
    Ops::makeEmpty(&e->element);

    // Update active Ranges.
    uint32_t pos = e - data;
    forEachRange<&Range::onRemove>(pos);

    // If many entries have been removed, try to shrink the table.
    if (hashBuckets() > initialBuckets() &&
        liveCount < dataLength * minDataFill()) {
      if (!rehash(hashShift + 1)) {
        return false;
      }
    }

    return true;
  }

  /*
   * Remove all entries.
   *
   * Returns false on OOM, leaving the OrderedHashTable and any live Ranges
   * in the old state.
   *
   * The effect on live Ranges is the same as removing all entries; in
   * particular, those Ranges are still live and will see any entries added
   * after a successful clear().
   */
  [[nodiscard]] bool clear() {
    if (dataLength != 0) {
      Data** oldHashTable = hashTable;
      Data* oldData = data;
      uint32_t oldHashBuckets = hashBuckets();
      uint32_t oldDataLength = dataLength;
      uint32_t oldDataCapacity = dataCapacity;

      hashTable = nullptr;
      if (!init()) {
        // init() only mutates members on success; see comment above.
        hashTable = oldHashTable;
        return false;
      }

      alloc.free_(oldHashTable, oldHashBuckets);
      freeData(oldData, oldDataLength, oldDataCapacity);
      forEachRange<&Range::onClear>();
    }

    MOZ_ASSERT(hashTable);
    MOZ_ASSERT(data);
    MOZ_ASSERT(dataLength == 0);
    MOZ_ASSERT(liveCount == 0);
    return true;
  }

  /*
   * Ranges are used to iterate over OrderedHashTables.
   *
   * Suppose 'Map' is some instance of OrderedHashMap, and 'map' is a Map.
   * Then you can walk all the key-value pairs like this:
   *
   *     for (Map::Range r = map.all(); !r.empty(); r.popFront()) {
   *         Map::Entry& pair = r.front();
   *         ... do something with pair ...
   *     }
   *
   * Ranges remain valid for the lifetime of the OrderedHashTable, even if
   * entries are added or removed or the table is resized. Don't do anything
   * to a Range, except destroy it, after the OrderedHashTable has been
   * destroyed. (We support destroying the two objects in either order to
   * humor the GC, bless its nondeterministic heart.)
   *
   * Warning: The behavior when the current front() entry is removed from the
   * table is subtly different from js::HashTable<>::Enum::removeFront()!
   * HashTable::Enum doesn't skip any entries when you removeFront() and then
   * popFront(). OrderedHashTable::Range does! (This is useful for using a
   * Range to implement JS Map.prototype.iterator.)
   *
   * The workaround is to call popFront() as soon as possible,
   * before there's any possibility of modifying the table:
   *
   *     for (Map::Range r = map.all(); !r.empty(); ) {
   *         Key key = r.front().key;         // this won't modify map
   *         Value val = r.front().value;     // this won't modify map
   *         r.popFront();
   *         // ...do things that might modify map...
   *     }
   */
  class Range {
    friend class OrderedHashTable;

    // Cannot be a reference since we need to be able to do
    // |offsetof(Range, ht)|.
    OrderedHashTable* ht;

    /* The index of front() within ht->data. */
    uint32_t i;

    /*
     * The number of nonempty entries in ht->data to the left of front().
     * This is used when the table is resized or compacted.
     */
    uint32_t count;

    /*
     * Links in the doubly-linked list of active Ranges on ht.
     *
     * prevp points to the previous Range's .next field;
     *   or to ht->ranges if this is the first Range in the list.
     * next points to the next Range;
     *   or nullptr if this is the last Range in the list.
     *
     * Invariant: *prevp == this.
     */
    Range** prevp;
    Range* next;

    /*
     * Create a Range over all the entries in ht.
     * (This is private on purpose. End users must use ht->all().)
     */
    Range(OrderedHashTable* ht, Range** listp)
        : ht(ht), i(0), count(0), prevp(listp), next(*listp) {
      *prevp = this;
      if (next) {
        next->prevp = &next;
      }
      seek();
      MOZ_ASSERT(valid());
    }

   public:
    Range(const Range& other, bool inNursery)
        : ht(other.ht),
          i(other.i),
          count(other.count),
          prevp(inNursery ? &ht->nurseryRanges : &ht->ranges),
          next(*prevp) {
      *prevp = this;
      if (next) {
        next->prevp = &next;
      }
      MOZ_ASSERT(valid());
    }

    ~Range() {
      if (!prevp) {
        // Head of removed nursery ranges.
        return;
      }
      *prevp = next;
      if (next) {
        next->prevp = prevp;
      }
    }

   protected:
    // Prohibit copy assignment.
    Range& operator=(const Range& other) = delete;

    void seek() {
      while (i < ht->dataLength &&
             Ops::isEmpty(Ops::getKey(ht->data[i].element))) {
        i++;
      }
    }

    /*
     * The hash table calls this when an entry is removed.
     * j is the index of the removed entry.
     */
    void onRemove(uint32_t j) {
      MOZ_ASSERT(valid());
      if (j < i) {
        count--;
      }
      if (j == i) {
        seek();
      }
    }

    /*
     * The hash table calls this when the table is resized or compacted.
     * Since |count| is the number of nonempty entries to the left of
     * front(), discarding the empty entries will not affect count, and it
     * will make i and count equal.
     */
    void onCompact() {
      MOZ_ASSERT(valid());
      i = count;
    }

    /* The hash table calls this when cleared. */
    void onClear() {
      MOZ_ASSERT(valid());
      i = count = 0;
    }

#ifdef DEBUG
    bool valid() const { return /* *prevp == this && */ next != this; }
#endif

    void onTableDestroyed() {
      MOZ_ASSERT(valid());
      prevp = &next;
      next = this;
      MOZ_ASSERT(!valid());
    }

   public:
    bool empty() const {
      MOZ_ASSERT(valid());
      return i >= ht->dataLength;
    }

    /*
     * Return the first element in the range. This must not be called if
     * this->empty().
     *
     * Warning: Removing an entry from the table also removes it from any
     * live Ranges, and a Range can become empty that way, rendering
     * front() invalid. If in doubt, check empty() before calling front().
     */
    const T& front() const {
      MOZ_ASSERT(valid());
      MOZ_ASSERT(!empty());
      return ht->data[i].element;
    }

    /*
     * Remove the first element from this range.
     * This must not be called if this->empty().
     *
     * Warning: Removing an entry from the table also removes it from any
     * live Ranges, and a Range can become empty that way, rendering
     * popFront() invalid. If in doubt, check empty() before calling
     * popFront().
     */
    void popFront() {
      MOZ_ASSERT(valid());
      MOZ_ASSERT(!empty());
      MOZ_ASSERT(!Ops::isEmpty(Ops::getKey(ht->data[i].element)));
      count++;
      i++;
      seek();
    }

    static size_t offsetOfHashTable() { return offsetof(Range, ht); }
    static size_t offsetOfI() { return offsetof(Range, i); }
    static size_t offsetOfCount() { return offsetof(Range, count); }
    static size_t offsetOfPrevP() { return offsetof(Range, prevp); }
    static size_t offsetOfNext() { return offsetof(Range, next); }

    static void onTableDestroyed(Range* range, uint32_t arg) {
      range->onTableDestroyed();
    }
    static void onRemove(Range* range, uint32_t arg) { range->onRemove(arg); }
    static void onClear(Range* range, uint32_t arg) { range->onClear(); }
    static void onCompact(Range* range, uint32_t arg) { range->onCompact(); }
  };

  class MutableRange : public Range {
    MutableRange(OrderedHashTable* ht, Range** listp) : Range(ht, listp) {}
    friend class OrderedHashTable;

   public:
    T& front() {
      MOZ_ASSERT(this->valid());
      MOZ_ASSERT(!this->empty());
      return this->ht->data[this->i].element;
    }

    void rekeyFront(const Key& k) {
      MOZ_ASSERT(this->valid());
      this->ht->rekey(&this->ht->data[this->i], k);
    }
  };

  Range all() const {
    // Range operates on a mutable table but its interface does not permit
    // modification of the contents of the table.
    auto* self = const_cast<OrderedHashTable*>(this);
    return Range(self, &self->ranges);
  }
  MutableRange mutableAll() { return MutableRange(this, &ranges); }

  void trace(JSTracer* trc) {
    for (uint32_t i = 0; i < dataLength; i++) {
      if (!Ops::isEmpty(Ops::getKey(data[i].element))) {
        Ops::trace(trc, this, i, data[i].element);
      }
    }
  }

  // For use by the implementation of Ops::trace.
  template <typename Key>
  void traceKey(JSTracer* trc, uint32_t index, Key& key) {
    MOZ_ASSERT(index < dataLength);
    using MutableKey = std::remove_const_t<Key>;
    using UnbarrieredKey = typename RemoveBarrier<MutableKey>::Type;
    UnbarrieredKey newKey = key;
    JS::GCPolicy<UnbarrieredKey>::trace(trc, &newKey, "OrderedHashMap key");
    if (newKey != key) {
      rekey(&data[index], newKey);
    }
  }
  template <typename Value>
  void traceValue(JSTracer* trc, Value& value) {
    JS::GCPolicy<Value>::trace(trc, &value, "OrderedHashMap value");
  }

  /*
   * Allocate a new Range, possibly in nursery memory. The buffer must be
   * large enough to hold a Range object.
   */
  Range* createRange(void* buffer, bool inNursery) const {
    auto* self = const_cast<OrderedHashTable*>(this);
    Range** listp = inNursery ? &self->nurseryRanges : &self->ranges;
    new (buffer) Range(self, listp);
    return static_cast<Range*>(buffer);
  }

  void destroyNurseryRanges() {
    if (nurseryRanges) {
      nurseryRanges->prevp = nullptr;
    }
    nurseryRanges = nullptr;
  }

#ifdef DEBUG
  bool hasNurseryRanges() const { return nurseryRanges; }
#endif

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
    Data* entry = lookup(current, currentHash);
    MOZ_ASSERT(entry);

    HashNumber oldHash = currentHash >> hashShift;
    HashNumber newHash = prepareHash(newKey) >> hashShift;

    entry->element = element;

    // Remove this entry from its old hash chain. (If this crashes
    // reading nullptr, it would mean we did not find this entry on
    // the hash chain where we expected it. That probably means the
    // key's hash code changed since it was inserted, breaking the
    // hash code invariant.)
    Data** ep = &hashTable[oldHash];
    while (*ep != entry) {
      ep = &(*ep)->chain;
    }
    *ep = entry->chain;

    // Add it to the new hash chain. We could just insert it at the
    // beginning of the chain. Instead, we do a bit of work to
    // preserve the invariant that hash chains always go in reverse
    // insertion order (descending memory order). No code currently
    // depends on this invariant, so it's fine to kill it if
    // needed.
    ep = &hashTable[newHash];
    while (*ep && *ep > entry) {
      ep = &(*ep)->chain;
    }
    entry->chain = *ep;
    *ep = entry;
  }

  static size_t offsetOfDataLength() {
    return offsetof(OrderedHashTable, dataLength);
  }
  static size_t offsetOfData() { return offsetof(OrderedHashTable, data); }
  static constexpr size_t offsetOfHashTable() {
    return offsetof(OrderedHashTable, hashTable);
  }
  static constexpr size_t offsetOfHashShift() {
    return offsetof(OrderedHashTable, hashShift);
  }
  static constexpr size_t offsetOfLiveCount() {
    return offsetof(OrderedHashTable, liveCount);
  }
  static constexpr size_t offsetOfDataElement() {
    static_assert(offsetof(Data, element) == 0,
                  "RangeFront and RangePopFront depend on offsetof(Data, "
                  "element) being 0");
    return offsetof(Data, element);
  }
  static constexpr size_t offsetOfDataChain() { return offsetof(Data, chain); }
  static constexpr size_t sizeofData() { return sizeof(Data); }

  static constexpr size_t offsetOfHcsK0() {
    return offsetof(OrderedHashTable, hcs) +
           mozilla::HashCodeScrambler::offsetOfMK0();
  }
  static constexpr size_t offsetOfHcsK1() {
    return offsetof(OrderedHashTable, hcs) +
           mozilla::HashCodeScrambler::offsetOfMK1();
  }

 private:
  /* Logarithm base 2 of the number of buckets in the hash table initially. */
  static uint32_t initialBucketsLog2() { return 1; }
  static uint32_t initialBuckets() { return 1 << initialBucketsLog2(); }

  /*
   * The maximum load factor (mean number of entries per bucket).
   * It is an invariant that
   *     dataCapacity == floor(hashBuckets() * fillFactor()).
   *
   * The fill factor should be between 2 and 4, and it should be chosen so that
   * the fill factor times sizeof(Data) is close to but <= a power of 2.
   * This fixed fill factor was chosen to make the size of the data
   * array, in bytes, close to a power of two when sizeof(T) is 16.
   */
  static constexpr double fillFactor() { return 8.0 / 3.0; }

  /*
   * The minimum permitted value of (liveCount / dataLength).
   * If that ratio drops below this value, we shrink the table.
   */
  static double minDataFill() { return 0.25; }

 public:
  HashNumber prepareHash(const Lookup& l) const {
    return mozilla::ScrambleHashCode(Ops::hash(l, hcs));
  }

 private:
  /* The size of hashTable, in elements. Always a power of two. */
  uint32_t hashBuckets() const {
    return 1 << (js::kHashNumberBits - hashShift);
  }

  static void destroyData(Data* data, uint32_t length) {
    for (Data* p = data + length; p != data;) {
      (--p)->~Data();
    }
  }

  void freeData(Data* data, uint32_t length, uint32_t capacity) {
    destroyData(data, length);
    alloc.free_(data, capacity);
  }

  Data* lookup(const Lookup& l, HashNumber h) {
    for (Data* e = hashTable[h >> hashShift]; e; e = e->chain) {
      if (Ops::match(Ops::getKey(e->element), l)) {
        return e;
      }
    }
    return nullptr;
  }

  const Data* lookup(const Lookup& l) const {
    return const_cast<OrderedHashTable*>(this)->lookup(l, prepareHash(l));
  }

  std::tuple<Data*, Data*> addEntry(HashNumber hash) {
    MOZ_ASSERT(dataLength < dataCapacity);
    hash >>= hashShift;
    liveCount++;
    Data* entry = &data[dataLength++];
    Data* chain = hashTable[hash];
    hashTable[hash] = entry;
    return std::make_tuple(entry, chain);
  }

  /* This is called after rehashing the table. */
  void compacted() {
    // If we had any empty entries, compacting may have moved live entries
    // to the left within |data|. Notify all live Ranges of the change.
    forEachRange<&Range::onCompact>();
  }

  /* Compact the entries in |data| and rehash them. */
  void rehashInPlace() {
    for (uint32_t i = 0, N = hashBuckets(); i < N; i++) {
      hashTable[i] = nullptr;
    }
    Data* wp = data;
    Data* end = data + dataLength;
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
    MOZ_ASSERT(wp == data + liveCount);

    while (wp != end) {
      (--end)->~Data();
    }
    dataLength = liveCount;
    compacted();
  }

  [[nodiscard]] bool rehashOnFull() {
    MOZ_ASSERT(dataLength == dataCapacity);

    // If the hashTable is more than 1/4 deleted data, simply rehash in
    // place to free up some space. Otherwise, grow the table.
    uint32_t newHashShift =
        liveCount >= dataCapacity * 0.75 ? hashShift - 1 : hashShift;
    return rehash(newHashShift);
  }

  /*
   * Grow, shrink, or compact both |hashTable| and |data|.
   *
   * On success, this returns true, dataLength == liveCount, and there are no
   * empty elements in data[0:dataLength]. On allocation failure, this
   * leaves everything as it was and returns false.
   */
  [[nodiscard]] bool rehash(uint32_t newHashShift) {
    // If the size of the table is not changing, rehash in place to avoid
    // allocating memory.
    if (newHashShift == hashShift) {
      rehashInPlace();
      return true;
    }

    // Ensure the new capacity fits into INT32_MAX.
    constexpr size_t maxCapacityLog2 =
        mozilla::tl::FloorLog2<size_t(INT32_MAX / fillFactor())>::value;
    static_assert(maxCapacityLog2 < kHashNumberBits);

    // Fail if |(js::kHashNumberBits - newHashShift) > maxCapacityLog2|.
    //
    // Reorder |kHashNumberBits| so both constants are on the right-hand side.
    if (MOZ_UNLIKELY(newHashShift < (js::kHashNumberBits - maxCapacityLog2))) {
      alloc.reportAllocOverflow();
      return false;
    }

    size_t newHashBuckets = size_t(1) << (js::kHashNumberBits - newHashShift);
    Data** newHashTable = alloc.template pod_malloc<Data*>(newHashBuckets);
    if (!newHashTable) {
      return false;
    }
    for (uint32_t i = 0; i < newHashBuckets; i++) {
      newHashTable[i] = nullptr;
    }

    uint32_t newCapacity = uint32_t(newHashBuckets * fillFactor());
    Data* newData = alloc.template pod_malloc<Data>(newCapacity);
    if (!newData) {
      alloc.free_(newHashTable, newHashBuckets);
      return false;
    }

    Data* wp = newData;
    Data* end = data + dataLength;
    for (Data* p = data; p != end; p++) {
      if (!Ops::isEmpty(Ops::getKey(p->element))) {
        HashNumber h = prepareHash(Ops::getKey(p->element)) >> newHashShift;
        new (wp) Data(std::move(p->element), newHashTable[h]);
        newHashTable[h] = wp;
        wp++;
      }
    }
    MOZ_ASSERT(wp == newData + liveCount);

    alloc.free_(hashTable, hashBuckets());
    freeData(data, dataLength, dataCapacity);

    hashTable = newHashTable;
    data = newData;
    dataLength = liveCount;
    dataCapacity = newCapacity;
    hashShift = newHashShift;
    MOZ_ASSERT(hashBuckets() == newHashBuckets);

    compacted();
    return true;
  }

  // Change the key of the front entry.
  //
  // This calls Ops::hash on both the current key and the new key. Ops::hash on
  // the current key must return the same hash code as when the entry was added
  // to the table.
  void rekey(Data* entry, const Key& k) {
    HashNumber oldHash = prepareHash(Ops::getKey(entry->element)) >> hashShift;
    HashNumber newHash = prepareHash(k) >> hashShift;
    Ops::setKey(entry->element, k);
    if (newHash != oldHash) {
      // Remove this entry from its old hash chain. (If this crashes reading
      // nullptr, it would mean we did not find this entry on the hash chain
      // where we expected it. That probably means the key's hash code changed
      // since it was inserted, breaking the hash code invariant.)
      Data** ep = &hashTable[oldHash];
      while (*ep != entry) {
        ep = &(*ep)->chain;
      }
      *ep = entry->chain;

      // Add it to the new hash chain. We could just insert it at the beginning
      // of the chain. Instead, we do a bit of work to preserve the invariant
      // that hash chains always go in reverse insertion order (descending
      // memory order). No code currently depends on this invariant, so it's
      // fine to kill it if needed.
      ep = &hashTable[newHash];
      while (*ep && *ep > entry) {
        ep = &(*ep)->chain;
      }
      entry->chain = *ep;
      *ep = entry;
    }
  }

  // Not copyable.
  OrderedHashTable& operator=(const OrderedHashTable&) = delete;
  OrderedHashTable(const OrderedHashTable&) = delete;
};

}  // namespace detail

template <class Key, class Value, class OrderedHashPolicy, class AllocPolicy>
class OrderedHashMap {
 public:
  class Entry {
    template <class, class, class>
    friend class detail::OrderedHashTable;
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
    Entry() : key(), value() {}
    explicit Entry(const Key& k) : key(k), value() {}
    template <typename V>
    Entry(const Key& k, V&& v) : key(k), value(std::forward<V>(v)) {}
    Entry(Entry&& rhs) : key(std::move(rhs.key)), value(std::move(rhs.value)) {}

    const Key key;
    Value value;

    static size_t offsetOfKey() { return offsetof(Entry, key); }
    static size_t offsetOfValue() { return offsetof(Entry, value); }
  };

 private:
  struct MapOps;
  using Impl = detail::OrderedHashTable<Entry, MapOps, AllocPolicy>;

  struct MapOps : OrderedHashPolicy {
    using KeyType = Key;
    static void makeEmpty(Entry* e) {
      OrderedHashPolicy::makeEmpty(const_cast<Key*>(&e->key));

      // Clear the value. Destroying it is another possibility, but that
      // would complicate class Entry considerably.
      e->value = Value();
    }
    static const Key& getKey(const Entry& e) { return e.key; }
    static void setKey(Entry& e, const Key& k) { const_cast<Key&>(e.key) = k; }
    static void trace(JSTracer* trc, Impl* table, uint32_t index,
                      Entry& entry) {
      table->traceKey(trc, index, entry.key);
      table->traceValue(trc, entry.value);
    }
  };

  Impl impl;

 public:
  using Lookup = typename Impl::Lookup;
  using Range = typename Impl::Range;
  using MutableRange = typename Impl::MutableRange;

  OrderedHashMap(AllocPolicy ap, mozilla::HashCodeScrambler hcs)
      : impl(std::move(ap), hcs) {}
  [[nodiscard]] bool init() { return impl.init(); }
  uint32_t count() const { return impl.count(); }
  bool has(const Lookup& key) const { return impl.has(key); }
  Range all() const { return impl.all(); }
  MutableRange mutableAll() { return impl.mutableAll(); }
  const Entry* get(const Lookup& key) const { return impl.get(key); }
  Entry* get(const Lookup& key) { return impl.get(key); }
  bool remove(const Lookup& key, bool* foundp) {
    return impl.remove(key, foundp);
  }
  // Remove an entry returned by get().
  bool remove(Entry* entry) {
    static_assert(offsetof(typename Impl::Data, element) == 0);
    auto* data = reinterpret_cast<typename Impl::Data*>(entry);
    return impl.remove(data);
  }
  [[nodiscard]] bool clear() { return impl.clear(); }

  template <typename K, typename V>
  [[nodiscard]] bool put(K&& key, V&& value) {
    return impl.put(Entry(std::forward<K>(key), std::forward<V>(value)));
  }

  template <typename K>
  [[nodiscard]] Entry* getOrAdd(K&& key) {
    return impl.getOrAdd(Entry(std::forward<K>(key)));
  }

  HashNumber hash(const Lookup& key) const { return impl.prepareHash(key); }

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

  Range* createRange(void* buffer, bool inNursery) const {
    return impl.createRange(buffer, inNursery);
  }

  void destroyNurseryRanges() { impl.destroyNurseryRanges(); }
#ifdef DEBUG
  bool hasNurseryRanges() const { return impl.hasNurseryRanges(); }
#endif

  void trace(JSTracer* trc) { impl.trace(trc); }

  static size_t offsetOfEntryKey() { return Entry::offsetOfKey(); }
  static size_t offsetOfImplDataLength() { return Impl::offsetOfDataLength(); }
  static size_t offsetOfImplData() { return Impl::offsetOfData(); }
  static constexpr size_t offsetOfImplHashTable() {
    return Impl::offsetOfHashTable();
  }
  static constexpr size_t offsetOfImplHashShift() {
    return Impl::offsetOfHashShift();
  }
  static constexpr size_t offsetOfImplLiveCount() {
    return Impl::offsetOfLiveCount();
  }
  static constexpr size_t offsetOfImplDataElement() {
    return Impl::offsetOfDataElement();
  }
  static constexpr size_t offsetOfImplDataChain() {
    return Impl::offsetOfDataChain();
  }
  static constexpr size_t sizeofImplData() { return Impl::sizeofData(); }

  static constexpr size_t offsetOfImplHcsK0() { return Impl::offsetOfHcsK0(); }
  static constexpr size_t offsetOfImplHcsK1() { return Impl::offsetOfHcsK1(); }

  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
    return impl.sizeOfExcludingThis(mallocSizeOf);
  }
  size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
    return mallocSizeOf(this) + sizeOfExcludingThis(mallocSizeOf);
  }
};

template <class T, class OrderedHashPolicy, class AllocPolicy>
class OrderedHashSet {
 private:
  struct SetOps;
  using Impl = detail::OrderedHashTable<T, SetOps, AllocPolicy>;

  struct SetOps : OrderedHashPolicy {
    using KeyType = const T;
    static const T& getKey(const T& v) { return v; }
    static void setKey(const T& e, const T& v) { const_cast<T&>(e) = v; }
    static void trace(JSTracer* trc, Impl* table, uint32_t index, T& entry) {
      table->traceKey(trc, index, entry);
    }
  };

  Impl impl;

 public:
  using Lookup = typename Impl::Lookup;
  using Range = typename Impl::Range;
  using MutableRange = typename Impl::MutableRange;

  explicit OrderedHashSet(AllocPolicy ap, mozilla::HashCodeScrambler hcs)
      : impl(std::move(ap), hcs) {}
  [[nodiscard]] bool init() { return impl.init(); }
  uint32_t count() const { return impl.count(); }
  bool has(const Lookup& value) const { return impl.has(value); }
  Range all() const { return impl.all(); }
  MutableRange mutableAll() { return impl.mutableAll(); }
  template <typename Input>
  [[nodiscard]] bool put(Input&& value) {
    return impl.put(std::forward<Input>(value));
  }
  bool remove(const Lookup& value, bool* foundp) {
    return impl.remove(value, foundp);
  }
  [[nodiscard]] bool clear() { return impl.clear(); }

  HashNumber hash(const Lookup& value) const { return impl.prepareHash(value); }

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

  Range* createRange(void* buffer, bool inNursery) const {
    return impl.createRange(buffer, inNursery);
  }

  void destroyNurseryRanges() { impl.destroyNurseryRanges(); }
#ifdef DEBUG
  bool hasNurseryRanges() const { return impl.hasNurseryRanges(); }
#endif

  void trace(JSTracer* trc) { impl.trace(trc); }

  static size_t offsetOfEntryKey() { return 0; }
  static size_t offsetOfImplDataLength() { return Impl::offsetOfDataLength(); }
  static size_t offsetOfImplData() { return Impl::offsetOfData(); }
  static constexpr size_t offsetOfImplHashTable() {
    return Impl::offsetOfHashTable();
  }
  static constexpr size_t offsetOfImplHashShift() {
    return Impl::offsetOfHashShift();
  }
  static constexpr size_t offsetOfImplLiveCount() {
    return Impl::offsetOfLiveCount();
  }
  static constexpr size_t offsetOfImplDataElement() {
    return Impl::offsetOfDataElement();
  }
  static constexpr size_t offsetOfImplDataChain() {
    return Impl::offsetOfDataChain();
  }
  static constexpr size_t sizeofImplData() { return Impl::sizeofData(); }

  static constexpr size_t offsetOfImplHcsK0() { return Impl::offsetOfHcsK0(); }
  static constexpr size_t offsetOfImplHcsK1() { return Impl::offsetOfHcsK1(); }

  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
    return impl.sizeOfExcludingThis(mallocSizeOf);
  }
  size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
    return mallocSizeOf(this) + sizeOfExcludingThis(mallocSizeOf);
  }
};

}  // namespace js

#endif /* ds_OrderedHashTable_h */
