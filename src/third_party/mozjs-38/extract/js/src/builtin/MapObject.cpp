/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/MapObject.h"

#include "mozilla/Move.h"

#include "jscntxt.h"
#include "jsiter.h"
#include "jsobj.h"

#include "gc/Marking.h"
#include "js/Utility.h"
#include "vm/GlobalObject.h"
#include "vm/Interpreter.h"

#include "jsobjinlines.h"

#include "vm/Interpreter-inl.h"
#include "vm/NativeObject-inl.h"

using namespace js;

using mozilla::ArrayLength;
using mozilla::Forward;
using mozilla::IsNaN;
using mozilla::Move;
using mozilla::NumberEqualsInt32;

using JS::DoubleNaNValue;
using JS::ForOfIterator;


/*** OrderedHashTable ****************************************************************************/

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
 * must additionally provide a distinguished "empty" key value and the
 * following static member functions:
 *     bool isEmpty(const Key&);
 *     void makeEmpty(Key*);
 */

namespace js {

namespace detail {

/*
 * detail::OrderedHashTable is the underlying data structure used to implement both
 * OrderedHashMap and OrderedHashSet. Programs should use one of those two
 * templates rather than OrderedHashTable.
 */
template <class T, class Ops, class AllocPolicy>
class OrderedHashTable
{
  public:
    typedef typename Ops::KeyType Key;
    typedef typename Ops::Lookup Lookup;

    struct Data
    {
        T element;
        Data* chain;

        Data(const T& e, Data* c) : element(e), chain(c) {}
        Data(T&& e, Data* c) : element(Move(e)), chain(c) {}
    };

    class Range;
    friend class Range;

  private:
    Data** hashTable;           // hash table (has hashBuckets() elements)
    Data* data;                 // data vector, an array of Data objects
                                // data[0:dataLength] are constructed
    uint32_t dataLength;        // number of constructed elements in data
    uint32_t dataCapacity;      // size of data, in elements
    uint32_t liveCount;         // dataLength less empty (removed) entries
    uint32_t hashShift;         // multiplicative hash shift
    Range* ranges;              // list of all live Ranges on this table
    AllocPolicy alloc;

  public:
    explicit OrderedHashTable(AllocPolicy& ap)
        : hashTable(nullptr), data(nullptr), dataLength(0), ranges(nullptr), alloc(ap) {}

    bool init() {
        MOZ_ASSERT(!hashTable, "init must be called at most once");

        uint32_t buckets = initialBuckets();
        Data** tableAlloc = alloc.template pod_malloc<Data*>(buckets);
        if (!tableAlloc)
            return false;
        for (uint32_t i = 0; i < buckets; i++)
            tableAlloc[i] = nullptr;

        uint32_t capacity = uint32_t(buckets * fillFactor());
        Data* dataAlloc = alloc.template pod_malloc<Data>(capacity);
        if (!dataAlloc) {
            alloc.free_(tableAlloc);
            return false;
        }

        // clear() requires that members are assigned only after all allocation
        // has succeeded, and that this->ranges is left untouched.
        hashTable = tableAlloc;
        data = dataAlloc;
        dataLength = 0;
        dataCapacity = capacity;
        liveCount = 0;
        hashShift = HashNumberSizeBits - initialBucketsLog2();
        MOZ_ASSERT(hashBuckets() == buckets);
        return true;
    }

    ~OrderedHashTable() {
        for (Range* r = ranges, *next; r; r = next) {
            next = r->next;
            r->onTableDestroyed();
        }
        alloc.free_(hashTable);
        freeData(data, dataLength);
    }

    /* Return the number of elements in the table. */
    uint32_t count() const { return liveCount; }

    /* True if any element matches l. */
    bool has(const Lookup& l) const {
        return lookup(l) != nullptr;
    }

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
    bool put(ElementInput&& element) {
        HashNumber h = prepareHash(Ops::getKey(element));
        if (Data* e = lookup(Ops::getKey(element), h)) {
            e->element = Forward<ElementInput>(element);
            return true;
        }

        if (dataLength == dataCapacity) {
            // If the hashTable is more than 1/4 deleted data, simply rehash in
            // place to free up some space. Otherwise, grow the table.
            uint32_t newHashShift = liveCount >= dataCapacity * 0.75 ? hashShift - 1 : hashShift;
            if (!rehash(newHashShift))
                return false;
        }

        h >>= hashShift;
        liveCount++;
        Data* e = &data[dataLength++];
        new (e) Data(Forward<ElementInput>(element), hashTable[h]);
        hashTable[h] = e;
        return true;
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
        liveCount--;
        Ops::makeEmpty(&e->element);

        // Update active Ranges.
        uint32_t pos = e - data;
        for (Range* r = ranges; r; r = r->next)
            r->onRemove(pos);

        // If many entries have been removed, try to shrink the table.
        if (hashBuckets() > initialBuckets() && liveCount < dataLength * minDataFill()) {
            if (!rehash(hashShift + 1))
                return false;
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
    bool clear() {
        if (dataLength != 0) {
            Data** oldHashTable = hashTable;
            Data* oldData = data;
            uint32_t oldDataLength = dataLength;

            hashTable = nullptr;
            if (!init()) {
                // init() only mutates members on success; see comment above.
                hashTable = oldHashTable;
                return false;
            }

            alloc.free_(oldHashTable);
            freeData(oldData, oldDataLength);
            for (Range* r = ranges; r; r = r->next)
                r->onClear();
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
    class Range
    {
        friend class OrderedHashTable;

        OrderedHashTable& ht;

        /* The index of front() within ht.data. */
        uint32_t i;

        /*
         * The number of nonempty entries in ht.data to the left of front().
         * This is used when the table is resized or compacted.
         */
        uint32_t count;

        /*
         * Links in the doubly-linked list of active Ranges on ht.
         *
         * prevp points to the previous Range's .next field;
         *   or to ht.ranges if this is the first Range in the list.
         * next points to the next Range;
         *   or nullptr if this is the last Range in the list.
         *
         * Invariant: *prevp == this.
         */
        Range** prevp;
        Range* next;

        /*
         * Create a Range over all the entries in ht.
         * (This is private on purpose. End users must use ht.all().)
         */
        explicit Range(OrderedHashTable& ht) : ht(ht), i(0), count(0), prevp(&ht.ranges), next(ht.ranges) {
            *prevp = this;
            if (next)
                next->prevp = &next;
            seek();
        }

      public:
        Range(const Range& other)
            : ht(other.ht), i(other.i), count(other.count), prevp(&ht.ranges), next(ht.ranges)
        {
            *prevp = this;
            if (next)
                next->prevp = &next;
        }

        ~Range() {
            *prevp = next;
            if (next)
                next->prevp = prevp;
        }

      private:
        // Prohibit copy assignment.
        Range& operator=(const Range& other) = delete;

        void seek() {
            while (i < ht.dataLength && Ops::isEmpty(Ops::getKey(ht.data[i].element)))
                i++;
        }

        /*
         * The hash table calls this when an entry is removed.
         * j is the index of the removed entry.
         */
        void onRemove(uint32_t j) {
            MOZ_ASSERT(valid());
            if (j < i)
                count--;
            if (j == i)
                seek();
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

        bool valid() const {
            return next != this;
        }

        void onTableDestroyed() {
            MOZ_ASSERT(valid());
            prevp = &next;
            next = this;
        }

      public:
        bool empty() const {
            MOZ_ASSERT(valid());
            return i >= ht.dataLength;
        }

        /*
         * Return the first element in the range. This must not be called if
         * this->empty().
         *
         * Warning: Removing an entry from the table also removes it from any
         * live Ranges, and a Range can become empty that way, rendering
         * front() invalid. If in doubt, check empty() before calling front().
         */
        T& front() {
            MOZ_ASSERT(valid());
            MOZ_ASSERT(!empty());
            return ht.data[i].element;
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
            MOZ_ASSERT(!Ops::isEmpty(Ops::getKey(ht.data[i].element)));
            count++;
            i++;
            seek();
        }

        /*
         * Change the key of the front entry.
         *
         * This calls Ops::hash on both the current key and the new key.
         * Ops::hash on the current key must return the same hash code as
         * when the entry was added to the table.
         */
        void rekeyFront(const Key& k) {
            MOZ_ASSERT(valid());
            Data& entry = ht.data[i];
            HashNumber oldHash = prepareHash(Ops::getKey(entry.element)) >> ht.hashShift;
            HashNumber newHash = prepareHash(k) >> ht.hashShift;
            Ops::setKey(entry.element, k);
            if (newHash != oldHash) {
                // Remove this entry from its old hash chain. (If this crashes
                // reading nullptr, it would mean we did not find this entry on
                // the hash chain where we expected it. That probably means the
                // key's hash code changed since it was inserted, breaking the
                // hash code invariant.)
                Data** ep = &ht.hashTable[oldHash];
                while (*ep != &entry)
                    ep = &(*ep)->chain;
                *ep = entry.chain;

                // Add it to the new hash chain. We could just insert it at the
                // beginning of the chain. Instead, we do a bit of work to
                // preserve the invariant that hash chains always go in reverse
                // insertion order (descending memory order). No code currently
                // depends on this invariant, so it's fine to kill it if
                // needed.
                ep = &ht.hashTable[newHash];
                while (*ep && *ep > &entry)
                    ep = &(*ep)->chain;
                entry.chain = *ep;
                *ep = &entry;
            }
        }
    };

    Range all() { return Range(*this); }

    /*
     * Change the value of the given key.
     *
     * This calls Ops::hash on both the current key and the new key.
     * Ops::hash on the current key must return the same hash code as
     * when the entry was added to the table.
     */
    void rekeyOneEntry(const Key& current, const Key& newKey, const T& element) {
        if (current == newKey)
            return;

        Data* entry = lookup(current, prepareHash(current));
        if (!entry)
            return;

        HashNumber oldHash = prepareHash(current) >> hashShift;
        HashNumber newHash = prepareHash(newKey) >> hashShift;

        entry->element = element;

        // Remove this entry from its old hash chain. (If this crashes
        // reading nullptr, it would mean we did not find this entry on
        // the hash chain where we expected it. That probably means the
        // key's hash code changed since it was inserted, breaking the
        // hash code invariant.)
        Data** ep = &hashTable[oldHash];
        while (*ep != entry)
            ep = &(*ep)->chain;
        *ep = entry->chain;

        // Add it to the new hash chain. We could just insert it at the
        // beginning of the chain. Instead, we do a bit of work to
        // preserve the invariant that hash chains always go in reverse
        // insertion order (descending memory order). No code currently
        // depends on this invariant, so it's fine to kill it if
        // needed.
        ep = &hashTable[newHash];
        while (*ep && *ep > entry)
            ep = &(*ep)->chain;
        entry->chain = *ep;
        *ep = entry;
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
    static double fillFactor() { return 8.0 / 3.0; }

    /*
     * The minimum permitted value of (liveCount / dataLength).
     * If that ratio drops below this value, we shrink the table.
     */
    static double minDataFill() { return 0.25; }

    static HashNumber prepareHash(const Lookup& l) {
        return ScrambleHashCode(Ops::hash(l));
    }

    /* The size of hashTable, in elements. Always a power of two. */
    uint32_t hashBuckets() const {
        return 1 << (HashNumberSizeBits - hashShift);
    }

    static void destroyData(Data* data, uint32_t length) {
        for (Data* p = data + length; p != data; )
            (--p)->~Data();
    }

    void freeData(Data* data, uint32_t length) {
        destroyData(data, length);
        alloc.free_(data);
    }

    Data* lookup(const Lookup& l, HashNumber h) {
        for (Data* e = hashTable[h >> hashShift]; e; e = e->chain) {
            if (Ops::match(Ops::getKey(e->element), l))
                return e;
        }
        return nullptr;
    }

    const Data* lookup(const Lookup& l) const {
        return const_cast<OrderedHashTable*>(this)->lookup(l, prepareHash(l));
    }

    /* This is called after rehashing the table. */
    void compacted() {
        // If we had any empty entries, compacting may have moved live entries
        // to the left within |data|. Notify all live Ranges of the change.
        for (Range* r = ranges; r; r = r->next)
            r->onCompact();
    }

    /* Compact the entries in |data| and rehash them. */
    void rehashInPlace() {
        for (uint32_t i = 0, N = hashBuckets(); i < N; i++)
            hashTable[i] = nullptr;
        Data* wp = data, *end = data + dataLength;
        for (Data* rp = data; rp != end; rp++) {
            if (!Ops::isEmpty(Ops::getKey(rp->element))) {
                HashNumber h = prepareHash(Ops::getKey(rp->element)) >> hashShift;
                if (rp != wp)
                    wp->element = Move(rp->element);
                wp->chain = hashTable[h];
                hashTable[h] = wp;
                wp++;
            }
        }
        MOZ_ASSERT(wp == data + liveCount);

        while (wp != end)
            (--end)->~Data();
        dataLength = liveCount;
        compacted();
    }

    /*
     * Grow, shrink, or compact both |hashTable| and |data|.
     *
     * On success, this returns true, dataLength == liveCount, and there are no
     * empty elements in data[0:dataLength]. On allocation failure, this
     * leaves everything as it was and returns false.
     */
    bool rehash(uint32_t newHashShift) {
        // If the size of the table is not changing, rehash in place to avoid
        // allocating memory.
        if (newHashShift == hashShift) {
            rehashInPlace();
            return true;
        }

        size_t newHashBuckets = 1 << (HashNumberSizeBits - newHashShift);
        Data** newHashTable = alloc.template pod_malloc<Data*>(newHashBuckets);
        if (!newHashTable)
            return false;
        for (uint32_t i = 0; i < newHashBuckets; i++)
            newHashTable[i] = nullptr;

        uint32_t newCapacity = uint32_t(newHashBuckets * fillFactor());
        Data* newData = alloc.template pod_malloc<Data>(newCapacity);
        if (!newData) {
            alloc.free_(newHashTable);
            return false;
        }

        Data* wp = newData;
        for (Data* p = data, *end = data + dataLength; p != end; p++) {
            if (!Ops::isEmpty(Ops::getKey(p->element))) {
                HashNumber h = prepareHash(Ops::getKey(p->element)) >> newHashShift;
                new (wp) Data(Move(p->element), newHashTable[h]);
                newHashTable[h] = wp;
                wp++;
            }
        }
        MOZ_ASSERT(wp == newData + liveCount);

        alloc.free_(hashTable);
        freeData(data, dataLength);

        hashTable = newHashTable;
        data = newData;
        dataLength = liveCount;
        dataCapacity = newCapacity;
        hashShift = newHashShift;
        MOZ_ASSERT(hashBuckets() == newHashBuckets);

        compacted();
        return true;
    }

    // Not copyable.
    OrderedHashTable& operator=(const OrderedHashTable&) = delete;
    OrderedHashTable(const OrderedHashTable&) = delete;
};

}  // namespace detail

template <class Key, class Value, class OrderedHashPolicy, class AllocPolicy>
class OrderedHashMap
{
  public:
    class Entry
    {
        template <class, class, class> friend class detail::OrderedHashTable;
        void operator=(const Entry& rhs) {
            const_cast<Key&>(key) = rhs.key;
            value = rhs.value;
        }

        void operator=(Entry&& rhs) {
            MOZ_ASSERT(this != &rhs, "self-move assignment is prohibited");
            const_cast<Key&>(key) = Move(rhs.key);
            value = Move(rhs.value);
        }

      public:
        Entry() : key(), value() {}
        Entry(const Key& k, const Value& v) : key(k), value(v) {}
        Entry(Entry&& rhs) : key(Move(rhs.key)), value(Move(rhs.value)) {}

        const Key key;
        Value value;
    };

  private:
    struct MapOps : OrderedHashPolicy
    {
        typedef Key KeyType;
        static void makeEmpty(Entry* e) {
            OrderedHashPolicy::makeEmpty(const_cast<Key*>(&e->key));

            // Clear the value. Destroying it is another possibility, but that
            // would complicate class Entry considerably.
            e->value = Value();
        }
        static const Key& getKey(const Entry& e) { return e.key; }
        static void setKey(Entry& e, const Key& k) { const_cast<Key&>(e.key) = k; }
    };

    typedef detail::OrderedHashTable<Entry, MapOps, AllocPolicy> Impl;
    Impl impl;

  public:
    typedef typename Impl::Range Range;

    explicit OrderedHashMap(AllocPolicy ap = AllocPolicy()) : impl(ap) {}
    bool init()                                     { return impl.init(); }
    uint32_t count() const                          { return impl.count(); }
    bool has(const Key& key) const                  { return impl.has(key); }
    Range all()                                     { return impl.all(); }
    const Entry* get(const Key& key) const          { return impl.get(key); }
    Entry* get(const Key& key)                      { return impl.get(key); }
    bool put(const Key& key, const Value& value)    { return impl.put(Entry(key, value)); }
    bool remove(const Key& key, bool* foundp)       { return impl.remove(key, foundp); }
    bool clear()                                    { return impl.clear(); }

    void rekeyOneEntry(const Key& current, const Key& newKey) {
        const Entry* e = get(current);
        if (!e)
            return;
        return impl.rekeyOneEntry(current, newKey, Entry(newKey, e->value));
    }
};

template <class T, class OrderedHashPolicy, class AllocPolicy>
class OrderedHashSet
{
  private:
    struct SetOps : OrderedHashPolicy
    {
        typedef const T KeyType;
        static const T& getKey(const T& v) { return v; }
        static void setKey(const T& e, const T& v) { const_cast<T&>(e) = v; }
    };

    typedef detail::OrderedHashTable<T, SetOps, AllocPolicy> Impl;
    Impl impl;

  public:
    typedef typename Impl::Range Range;

    explicit OrderedHashSet(AllocPolicy ap = AllocPolicy()) : impl(ap) {}
    bool init()                                     { return impl.init(); }
    uint32_t count() const                          { return impl.count(); }
    bool has(const T& value) const                  { return impl.has(value); }
    Range all()                                     { return impl.all(); }
    bool put(const T& value)                        { return impl.put(value); }
    bool remove(const T& value, bool* foundp)       { return impl.remove(value, foundp); }
    bool clear()                                    { return impl.clear(); }

    void rekeyOneEntry(const T& current, const T& newKey) {
        return impl.rekeyOneEntry(current, newKey, newKey);
    }
};

}  // namespace js


/*** HashableValue *******************************************************************************/

bool
HashableValue::setValue(JSContext* cx, HandleValue v)
{
    if (v.isString()) {
        // Atomize so that hash() and operator==() are fast and infallible.
        JSString* str = AtomizeString(cx, v.toString(), DoNotInternAtom);
        if (!str)
            return false;
        value = StringValue(str);
    } else if (v.isDouble()) {
        double d = v.toDouble();
        int32_t i;
        if (NumberEqualsInt32(d, &i)) {
            // Normalize int32_t-valued doubles to int32_t for faster hashing and testing.
            value = Int32Value(i);
        } else if (IsNaN(d)) {
            // NaNs with different bits must hash and test identically.
            value = DoubleNaNValue();
        } else {
            value = v;
        }
    } else {
        value = v;
    }

    MOZ_ASSERT(value.isUndefined() || value.isNull() || value.isBoolean() || value.isNumber() ||
               value.isString() || value.isSymbol() || value.isObject());
    return true;
}

HashNumber
HashableValue::hash() const
{
    // HashableValue::setValue normalizes values so that the SameValue relation
    // on HashableValues is the same as the == relationship on
    // value.data.asBits.
    return value.asRawBits();
}

bool
HashableValue::operator==(const HashableValue& other) const
{
    // Two HashableValues are equal if they have equal bits.
    bool b = (value.asRawBits() == other.value.asRawBits());

#ifdef DEBUG
    bool same;
    PerThreadData* data = TlsPerThreadData.get();
    RootedValue valueRoot(data, value);
    RootedValue otherRoot(data, other.value);
    MOZ_ASSERT(SameValue(nullptr, valueRoot, otherRoot, &same));
    MOZ_ASSERT(same == b);
#endif
    return b;
}

HashableValue
HashableValue::mark(JSTracer* trc) const
{
    HashableValue hv(*this);
    trc->setTracingLocation((void*)this);
    gc::MarkValue(trc, &hv.value, "key");
    return hv;
}


/*** MapIterator *********************************************************************************/

namespace {

class MapIteratorObject : public NativeObject
{
  public:
    static const Class class_;

    enum { TargetSlot, KindSlot, RangeSlot, SlotCount };
    static const JSFunctionSpec methods[];
    static MapIteratorObject* create(JSContext* cx, HandleObject mapobj, ValueMap* data,
                                     MapObject::IteratorKind kind);
    static bool next(JSContext* cx, unsigned argc, Value* vp);
    static void finalize(FreeOp* fop, JSObject* obj);

  private:
    static inline bool is(HandleValue v);
    inline ValueMap::Range* range();
    inline MapObject::IteratorKind kind() const;
    static bool next_impl(JSContext* cx, CallArgs args);
};

} /* anonymous namespace */

const Class MapIteratorObject::class_ = {
    "Map Iterator",
    JSCLASS_IMPLEMENTS_BARRIERS |
    JSCLASS_HAS_RESERVED_SLOTS(MapIteratorObject::SlotCount),
    nullptr, /* addProperty */
    nullptr, /* delProperty */
    nullptr, /* getProperty */
    nullptr, /* setProperty */
    nullptr, /* enumerate */
    nullptr, /* resolve */
    nullptr, /* convert */
    MapIteratorObject::finalize
};

const JSFunctionSpec MapIteratorObject::methods[] = {
    JS_SELF_HOSTED_SYM_FN(iterator, "IteratorIdentity", 0, 0),
    JS_FN("next", next, 0, 0),
    JS_FS_END
};

inline ValueMap::Range*
MapIteratorObject::range()
{
    return static_cast<ValueMap::Range*>(getSlot(RangeSlot).toPrivate());
}

inline MapObject::IteratorKind
MapIteratorObject::kind() const
{
    int32_t i = getSlot(KindSlot).toInt32();
    MOZ_ASSERT(i == MapObject::Keys || i == MapObject::Values || i == MapObject::Entries);
    return MapObject::IteratorKind(i);
}

bool
GlobalObject::initMapIteratorProto(JSContext* cx, Handle<GlobalObject*> global)
{
    Rooted<JSObject*> base(cx, GlobalObject::getOrCreateIteratorPrototype(cx, global));
    if (!base)
        return false;
    Rooted<MapIteratorObject*> proto(cx,
        NewObjectWithGivenProto<MapIteratorObject>(cx, base, global));
    if (!proto)
        return false;
    proto->setSlot(MapIteratorObject::RangeSlot, PrivateValue(nullptr));
    if (!JS_DefineFunctions(cx, proto, MapIteratorObject::methods))
        return false;
    global->setReservedSlot(MAP_ITERATOR_PROTO, ObjectValue(*proto));
    return true;
}

MapIteratorObject*
MapIteratorObject::create(JSContext* cx, HandleObject mapobj, ValueMap* data,
                          MapObject::IteratorKind kind)
{
    Rooted<GlobalObject*> global(cx, &mapobj->global());
    Rooted<JSObject*> proto(cx, GlobalObject::getOrCreateMapIteratorPrototype(cx, global));
    if (!proto)
        return nullptr;

    ValueMap::Range* range = cx->new_<ValueMap::Range>(data->all());
    if (!range)
        return nullptr;

    MapIteratorObject* iterobj = NewObjectWithGivenProto<MapIteratorObject>(cx, proto, global);
    if (!iterobj) {
        js_delete(range);
        return nullptr;
    }
    iterobj->setSlot(TargetSlot, ObjectValue(*mapobj));
    iterobj->setSlot(KindSlot, Int32Value(int32_t(kind)));
    iterobj->setSlot(RangeSlot, PrivateValue(range));
    return iterobj;
}

void
MapIteratorObject::finalize(FreeOp* fop, JSObject* obj)
{
    fop->delete_(obj->as<MapIteratorObject>().range());
}

bool
MapIteratorObject::is(HandleValue v)
{
    return v.isObject() && v.toObject().hasClass(&class_);
}

bool
MapIteratorObject::next_impl(JSContext* cx, CallArgs args)
{
    MapIteratorObject& thisobj = args.thisv().toObject().as<MapIteratorObject>();
    ValueMap::Range* range = thisobj.range();
    RootedValue value(cx);
    bool done;

    if (!range || range->empty()) {
        js_delete(range);
        thisobj.setReservedSlot(RangeSlot, PrivateValue(nullptr));
        value.setUndefined();
        done = true;
    } else {
        switch (thisobj.kind()) {
          case MapObject::Keys:
            value = range->front().key.get();
            break;

          case MapObject::Values:
            value = range->front().value;
            break;

          case MapObject::Entries: {
            JS::AutoValueArray<2> pair(cx);
            pair[0].set(range->front().key.get());
            pair[1].set(range->front().value);

            JSObject* pairobj = NewDenseCopiedArray(cx, pair.length(), pair.begin());
            if (!pairobj)
                return false;
            value.setObject(*pairobj);
            break;
          }
        }
        range->popFront();
        done = false;
    }

    RootedObject result(cx, CreateItrResultObject(cx, value, done));
    if (!result)
        return false;
    args.rval().setObject(*result);

    return true;
}

bool
MapIteratorObject::next(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod(cx, is, next_impl, args);
}


/*** Map *****************************************************************************************/

const Class MapObject::class_ = {
    "Map",
    JSCLASS_HAS_PRIVATE | JSCLASS_IMPLEMENTS_BARRIERS |
    JSCLASS_HAS_CACHED_PROTO(JSProto_Map),
    nullptr, // addProperty
    nullptr, // delProperty
    nullptr, // getProperty
    nullptr, // setProperty
    nullptr, // enumerate
    nullptr, // resolve
    nullptr, // convert
    finalize,
    nullptr, // call
    nullptr, // hasInstance
    nullptr, // construct
    mark
};

const JSPropertySpec MapObject::properties[] = {
    JS_PSG("size", size, 0),
    JS_PS_END
};

const JSFunctionSpec MapObject::methods[] = {
    JS_FN("get", get, 1, 0),
    JS_FN("has", has, 1, 0),
    JS_FN("set", set, 2, 0),
    JS_FN("delete", delete_, 1, 0),
    JS_FN("keys", keys, 0, 0),
    JS_FN("values", values, 0, 0),
    JS_FN("clear", clear, 0, 0),
    JS_SELF_HOSTED_FN("forEach", "MapForEach", 2, 0),
    JS_FS_END
};

static JSObject*
InitClass(JSContext* cx, Handle<GlobalObject*> global, const Class* clasp, JSProtoKey key, Native construct,
          const JSPropertySpec* properties, const JSFunctionSpec* methods)
{
    RootedNativeObject proto(cx, global->createBlankPrototype(cx, clasp));
    if (!proto)
        return nullptr;
    proto->setPrivate(nullptr);

    Rooted<JSFunction*> ctor(cx, global->createConstructor(cx, construct, ClassName(key, cx), 1));
    if (!ctor ||
        !LinkConstructorAndPrototype(cx, ctor, proto) ||
        !DefinePropertiesAndFunctions(cx, proto, properties, methods) ||
        !GlobalObject::initBuiltinConstructor(cx, global, key, ctor, proto))
    {
        return nullptr;
    }
    return proto;
}

JSObject*
MapObject::initClass(JSContext* cx, JSObject* obj)
{
    Rooted<GlobalObject*> global(cx, &obj->as<GlobalObject>());
    RootedObject proto(cx,
        InitClass(cx, global, &class_, JSProto_Map, construct, properties, methods));
    if (proto) {
        // Define the "entries" method.
        JSFunction* fun = JS_DefineFunction(cx, proto, "entries", entries, 0, 0);
        if (!fun)
            return nullptr;

        // Define its alias.
        RootedValue funval(cx, ObjectValue(*fun));
        RootedId iteratorId(cx, SYMBOL_TO_JSID(cx->wellKnownSymbols().iterator));
        if (!JS_DefinePropertyById(cx, proto, iteratorId, funval, 0))
            return nullptr;
    }
    return proto;
}

template <class Range>
static void
MarkKey(Range& r, const HashableValue& key, JSTracer* trc)
{
    HashableValue newKey = key.mark(trc);

    if (newKey.get() != key.get()) {
        // The hash function only uses the bits of the Value, so it is safe to
        // rekey even when the object or string has been modified by the GC.
        r.rekeyFront(newKey);
    }
}

void
MapObject::mark(JSTracer* trc, JSObject* obj)
{
    if (ValueMap* map = obj->as<MapObject>().getData()) {
        for (ValueMap::Range r = map->all(); !r.empty(); r.popFront()) {
            MarkKey(r, r.front().key, trc);
            gc::MarkValue(trc, &r.front().value, "value");
        }
    }
}

struct UnbarrieredHashPolicy {
    typedef Value Lookup;
    static HashNumber hash(const Lookup& v) { return v.asRawBits(); }
    static bool match(const Value& k, const Lookup& l) { return k == l; }
    static bool isEmpty(const Value& v) { return v.isMagic(JS_HASH_KEY_EMPTY); }
    static void makeEmpty(Value* vp) { vp->setMagic(JS_HASH_KEY_EMPTY); }
};

template <typename TableType>
class OrderedHashTableRef : public gc::BufferableRef
{
    TableType* table;
    Value key;

  public:
    explicit OrderedHashTableRef(TableType* t, const Value& k) : table(t), key(k) {}

    void mark(JSTracer* trc) {
        MOZ_ASSERT(UnbarrieredHashPolicy::hash(key) ==
                   HashableValue::Hasher::hash(*reinterpret_cast<HashableValue*>(&key)));
        Value prior = key;
        gc::MarkValueUnbarriered(trc, &key, "ordered hash table key");
        table->rekeyOneEntry(prior, key);
    }
};

inline static void
WriteBarrierPost(JSRuntime* rt, ValueMap* map, const Value& key)
{
    typedef OrderedHashMap<Value, Value, UnbarrieredHashPolicy, RuntimeAllocPolicy> UnbarrieredMap;
    if (MOZ_UNLIKELY(key.isObject() && IsInsideNursery(&key.toObject()))) {
        rt->gc.storeBuffer.putGeneric(OrderedHashTableRef<UnbarrieredMap>(
                    reinterpret_cast<UnbarrieredMap*>(map), key));
    }
}

inline static void
WriteBarrierPost(JSRuntime* rt, ValueSet* set, const Value& key)
{
    typedef OrderedHashSet<Value, UnbarrieredHashPolicy, RuntimeAllocPolicy> UnbarrieredSet;
    if (MOZ_UNLIKELY(key.isObject() && IsInsideNursery(&key.toObject()))) {
        rt->gc.storeBuffer.putGeneric(OrderedHashTableRef<UnbarrieredSet>(
                    reinterpret_cast<UnbarrieredSet*>(set), key));
    }
}

bool
MapObject::getKeysAndValuesInterleaved(JSContext* cx, HandleObject obj,
                                       JS::AutoValueVector* entries)
{
    ValueMap* map = obj->as<MapObject>().getData();
    if (!map)
        return false;

    for (ValueMap::Range r = map->all(); !r.empty(); r.popFront()) {
        if (!entries->append(r.front().key.get()) ||
            !entries->append(r.front().value))
        {
            return false;
        }
    }

    return true;
}

bool
MapObject::set(JSContext* cx, HandleObject obj, HandleValue k, HandleValue v)
{
    ValueMap* map = obj->as<MapObject>().getData();
    if (!map)
        return false;

    AutoHashableValueRooter key(cx);
    if (!key.setValue(cx, k))
        return false;

    RelocatableValue rval(v);
    if (!map->put(key, rval)) {
        js_ReportOutOfMemory(cx);
        return false;
    }
    WriteBarrierPost(cx->runtime(), map, key.get());
    return true;
}

MapObject*
MapObject::create(JSContext* cx)
{
    Rooted<MapObject*> obj(cx, NewBuiltinClassInstance<MapObject>(cx));
    if (!obj)
        return nullptr;

    ValueMap* map = cx->new_<ValueMap>(cx->runtime());
    if (!map || !map->init()) {
        js_delete(map);
        js_ReportOutOfMemory(cx);
        return nullptr;
    }

    obj->setPrivate(map);
    return obj;
}

void
MapObject::finalize(FreeOp* fop, JSObject* obj)
{
    if (ValueMap* map = obj->as<MapObject>().getData())
        fop->delete_(map);
}

bool
MapObject::construct(JSContext* cx, unsigned argc, Value* vp)
{
    Rooted<MapObject*> obj(cx, MapObject::create(cx));
    if (!obj)
        return false;

    CallArgs args = CallArgsFromVp(argc, vp);

    // FIXME: bug 1083752
    if (!WarnIfNotConstructing(cx, args, "Map"))
        return false;

    if (!args.get(0).isNullOrUndefined()) {
        RootedValue adderVal(cx);
        if (!GetProperty(cx, obj, obj, cx->names().set, &adderVal))
            return false;

        if (!IsCallable(adderVal))
            return ReportIsNotFunction(cx, adderVal);

        bool isOriginalAdder = IsNativeFunction(adderVal, MapObject::set);
        RootedValue mapVal(cx, ObjectValue(*obj));
        FastInvokeGuard fig(cx, adderVal);
        InvokeArgs& args2 = fig.args();

        ForOfIterator iter(cx);
        if (!iter.init(args[0]))
            return false;
        RootedValue pairVal(cx);
        RootedObject pairObj(cx);
        AutoHashableValueRooter hkey(cx);
        ValueMap* map = obj->getData();
        while (true) {
            bool done;
            if (!iter.next(&pairVal, &done))
                return false;
            if (done)
                break;
            if (!pairVal.isObject()) {
                JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr,
                                     JSMSG_INVALID_MAP_ITERABLE, "Map");
                return false;
            }

            pairObj = &pairVal.toObject();
            if (!pairObj)
                return false;

            RootedValue key(cx);
            if (!GetElement(cx, pairObj, pairObj, 0, &key))
                return false;

            RootedValue val(cx);
            if (!GetElement(cx, pairObj, pairObj, 1, &val))
                return false;

            if (isOriginalAdder) {
                if (!hkey.setValue(cx, key))
                    return false;

                RelocatableValue rval(val);
                if (!map->put(hkey, rval)) {
                    js_ReportOutOfMemory(cx);
                    return false;
                }
                WriteBarrierPost(cx->runtime(), map, key);
            } else {
                if (!args2.init(2))
                    return false;

                args2.setCallee(adderVal);
                args2.setThis(mapVal);
                args2[0].set(key);
                args2[1].set(val);

                if (!fig.invoke(cx))
                    return false;
            }
        }
    }

    args.rval().setObject(*obj);
    return true;
}

bool
MapObject::is(HandleValue v)
{
    return v.isObject() && v.toObject().hasClass(&class_) && v.toObject().as<MapObject>().getPrivate();
}

bool
MapObject::is(HandleObject o)
{
    return o->hasClass(&class_) && o->as<MapObject>().getPrivate();
}

#define ARG0_KEY(cx, args, key)                                               \
    AutoHashableValueRooter key(cx);                                          \
    if (args.length() > 0 && !key.setValue(cx, args[0]))                      \
        return false

ValueMap&
MapObject::extract(HandleObject o)
{
    MOZ_ASSERT(o->hasClass(&MapObject::class_));
    return *o->as<MapObject>().getData();
}

ValueMap&
MapObject::extract(CallReceiver call)
{
    MOZ_ASSERT(call.thisv().isObject());
    MOZ_ASSERT(call.thisv().toObject().hasClass(&MapObject::class_));
    return *call.thisv().toObject().as<MapObject>().getData();
}

uint32_t
MapObject::size(JSContext* cx, HandleObject obj)
{
    MOZ_ASSERT(MapObject::is(obj));
    ValueMap& map = extract(obj);
    static_assert(sizeof(map.count()) <= sizeof(uint32_t),
                  "map count must be precisely representable as a JS number");
    return map.count();
}

bool
MapObject::size_impl(JSContext* cx, CallArgs args)
{
    RootedObject obj(cx, &args.thisv().toObject());
    args.rval().setNumber(size(cx, obj));
    return true;
}

bool
MapObject::size(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<MapObject::is, MapObject::size_impl>(cx, args);
}

bool
MapObject::get(JSContext* cx, HandleObject obj,
               HandleValue key, MutableHandleValue rval)
{
    MOZ_ASSERT(MapObject::is(obj));

    ValueMap& map = extract(obj);
    AutoHashableValueRooter k(cx);

    if (!k.setValue(cx, key))
        return false;

    if (ValueMap::Entry* p = map.get(k))
        rval.set(p->value);
    else
        rval.setUndefined();

    return true;
}

bool
MapObject::get_impl(JSContext* cx, CallArgs args)
{
    RootedObject obj(cx, &args.thisv().toObject());
    return get(cx, obj, args.get(0), args.rval());
}

bool
MapObject::get(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<MapObject::is, MapObject::get_impl>(cx, args);
}

bool
MapObject::has(JSContext* cx, HandleObject obj, HandleValue key, bool* rval)
{
    MOZ_ASSERT(MapObject::is(obj));

    ValueMap& map = extract(obj);
    AutoHashableValueRooter k(cx);

    if (!k.setValue(cx, key))
        return false;

    *rval = map.has(k);
    return true;
}

bool
MapObject::has_impl(JSContext* cx, CallArgs args)
{
    bool found;
    RootedObject obj(cx, &args.thisv().toObject());
    if (has(cx, obj, args.get(0), &found)) {
        args.rval().setBoolean(found);
        return true;
    }
    return false;
}

bool
MapObject::has(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<MapObject::is, MapObject::has_impl>(cx, args);
}

bool
MapObject::set_impl(JSContext* cx, CallArgs args)
{
    MOZ_ASSERT(MapObject::is(args.thisv()));

    ValueMap& map = extract(args);
    ARG0_KEY(cx, args, key);
    RelocatableValue rval(args.get(1));
    if (!map.put(key, rval)) {
        js_ReportOutOfMemory(cx);
        return false;
    }
    WriteBarrierPost(cx->runtime(), &map, key.get());
    args.rval().set(args.thisv());
    return true;
}

bool
MapObject::set(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<MapObject::is, MapObject::set_impl>(cx, args);
}

bool
MapObject::delete_impl(JSContext* cx, CallArgs args)
{
    // MapObject::mark does not mark deleted entries. Incremental GC therefore
    // requires that no RelocatableValue objects pointing to heap values be
    // left alive in the ValueMap.
    //
    // OrderedHashMap::remove() doesn't destroy the removed entry. It merely
    // calls OrderedHashMap::MapOps::makeEmpty. But that is sufficient, because
    // makeEmpty clears the value by doing e->value = Value(), and in the case
    // of a ValueMap, Value() means RelocatableValue(), which is the same as
    // RelocatableValue(UndefinedValue()).
    MOZ_ASSERT(MapObject::is(args.thisv()));

    ValueMap& map = extract(args);
    ARG0_KEY(cx, args, key);
    bool found;
    if (!map.remove(key, &found)) {
        js_ReportOutOfMemory(cx);
        return false;
    }
    args.rval().setBoolean(found);
    return true;
}

bool
MapObject::delete_(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<MapObject::is, MapObject::delete_impl>(cx, args);
}

bool
MapObject::iterator(JSContext* cx, IteratorKind kind,
                    HandleObject obj, MutableHandleValue iter)
{
    MOZ_ASSERT(MapObject::is(obj));
    ValueMap& map = extract(obj);
    Rooted<JSObject*> iterobj(cx, MapIteratorObject::create(cx, obj, &map, kind));
    return iterobj && (iter.setObject(*iterobj), true);
}

bool
MapObject::iterator_impl(JSContext* cx, CallArgs args, IteratorKind kind)
{
    RootedObject obj(cx, &args.thisv().toObject());
    return iterator(cx, kind, obj, args.rval());
}

bool
MapObject::keys_impl(JSContext* cx, CallArgs args)
{
    return iterator_impl(cx, args, Keys);
}

bool
MapObject::keys(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod(cx, is, keys_impl, args);
}

bool
MapObject::values_impl(JSContext* cx, CallArgs args)
{
    return iterator_impl(cx, args, Values);
}

bool
MapObject::values(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod(cx, is, values_impl, args);
}

bool
MapObject::entries_impl(JSContext* cx, CallArgs args)
{
    return iterator_impl(cx, args, Entries);
}

bool
MapObject::entries(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod(cx, is, entries_impl, args);
}

bool
MapObject::clear_impl(JSContext* cx, CallArgs args)
{
    RootedObject obj(cx, &args.thisv().toObject());
    args.rval().setUndefined();
    return clear(cx, obj);
}

bool
MapObject::clear(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod(cx, is, clear_impl, args);
}

bool
MapObject::clear(JSContext* cx, HandleObject obj)
{
    MOZ_ASSERT(MapObject::is(obj));
    ValueMap& map = extract(obj);
    if (!map.clear()) {
        js_ReportOutOfMemory(cx);
        return false;
    }
    return true;
}

JSObject*
js_InitMapClass(JSContext* cx, HandleObject obj)
{
    return MapObject::initClass(cx, obj);
}


/*** SetIterator *********************************************************************************/

namespace {

class SetIteratorObject : public NativeObject
{
  public:
    static const Class class_;

    enum { TargetSlot, KindSlot, RangeSlot, SlotCount };
    static const JSFunctionSpec methods[];
    static SetIteratorObject* create(JSContext* cx, HandleObject setobj, ValueSet* data,
                                     SetObject::IteratorKind kind);
    static bool next(JSContext* cx, unsigned argc, Value* vp);
    static void finalize(FreeOp* fop, JSObject* obj);

  private:
    static inline bool is(HandleValue v);
    inline ValueSet::Range* range();
    inline SetObject::IteratorKind kind() const;
    static bool next_impl(JSContext* cx, CallArgs args);
};

} /* anonymous namespace */

const Class SetIteratorObject::class_ = {
    "Set Iterator",
    JSCLASS_IMPLEMENTS_BARRIERS |
    JSCLASS_HAS_RESERVED_SLOTS(SetIteratorObject::SlotCount),
    nullptr, /* addProperty */
    nullptr, /* delProperty */
    nullptr, /* getProperty */
    nullptr, /* setProperty */
    nullptr, /* enumerate */
    nullptr, /* resolve */
    nullptr, /* convert */
    SetIteratorObject::finalize
};

const JSFunctionSpec SetIteratorObject::methods[] = {
    JS_SELF_HOSTED_SYM_FN(iterator, "IteratorIdentity", 0, 0),
    JS_FN("next", next, 0, 0),
    JS_FS_END
};

inline ValueSet::Range*
SetIteratorObject::range()
{
    return static_cast<ValueSet::Range*>(getSlot(RangeSlot).toPrivate());
}

inline SetObject::IteratorKind
SetIteratorObject::kind() const
{
    int32_t i = getSlot(KindSlot).toInt32();
    MOZ_ASSERT(i == SetObject::Values || i == SetObject::Entries);
    return SetObject::IteratorKind(i);
}

bool
GlobalObject::initSetIteratorProto(JSContext* cx, Handle<GlobalObject*> global)
{
    Rooted<JSObject*> base(cx, GlobalObject::getOrCreateIteratorPrototype(cx, global));
    if (!base)
        return false;
    Rooted<SetIteratorObject*> proto(cx,
        NewObjectWithGivenProto<SetIteratorObject>(cx, base, global));
    if (!proto)
        return false;
    proto->setSlot(SetIteratorObject::RangeSlot, PrivateValue(nullptr));
    if (!JS_DefineFunctions(cx, proto, SetIteratorObject::methods))
        return false;
    global->setReservedSlot(SET_ITERATOR_PROTO, ObjectValue(*proto));
    return true;
}

SetIteratorObject*
SetIteratorObject::create(JSContext* cx, HandleObject setobj, ValueSet* data,
                          SetObject::IteratorKind kind)
{
    Rooted<GlobalObject*> global(cx, &setobj->global());
    Rooted<JSObject*> proto(cx, GlobalObject::getOrCreateSetIteratorPrototype(cx, global));
    if (!proto)
        return nullptr;

    ValueSet::Range* range = cx->new_<ValueSet::Range>(data->all());
    if (!range)
        return nullptr;

    SetIteratorObject* iterobj = NewObjectWithGivenProto<SetIteratorObject>(cx, proto, global);
    if (!iterobj) {
        js_delete(range);
        return nullptr;
    }
    iterobj->setSlot(TargetSlot, ObjectValue(*setobj));
    iterobj->setSlot(KindSlot, Int32Value(int32_t(kind)));
    iterobj->setSlot(RangeSlot, PrivateValue(range));
    return iterobj;
}

void
SetIteratorObject::finalize(FreeOp* fop, JSObject* obj)
{
    fop->delete_(obj->as<SetIteratorObject>().range());
}

bool
SetIteratorObject::is(HandleValue v)
{
    return v.isObject() && v.toObject().is<SetIteratorObject>();
}

bool
SetIteratorObject::next_impl(JSContext* cx, CallArgs args)
{
    SetIteratorObject& thisobj = args.thisv().toObject().as<SetIteratorObject>();
    ValueSet::Range* range = thisobj.range();
    RootedValue value(cx);
    bool done;

    if (!range || range->empty()) {
        js_delete(range);
        thisobj.setReservedSlot(RangeSlot, PrivateValue(nullptr));
        value.setUndefined();
        done = true;
    } else {
        switch (thisobj.kind()) {
          case SetObject::Values:
            value = range->front().get();
            break;

          case SetObject::Entries: {
            JS::AutoValueArray<2> pair(cx);
            pair[0].set(range->front().get());
            pair[1].set(range->front().get());

            JSObject* pairObj = NewDenseCopiedArray(cx, 2, pair.begin());
            if (!pairObj)
              return false;
            value.setObject(*pairObj);
            break;
          }
        }
        range->popFront();
        done = false;
    }

    RootedObject result(cx, CreateItrResultObject(cx, value, done));
    if (!result)
        return false;
    args.rval().setObject(*result);

    return true;
}

bool
SetIteratorObject::next(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod(cx, is, next_impl, args);
}


/*** Set *****************************************************************************************/

const Class SetObject::class_ = {
    "Set",
    JSCLASS_HAS_PRIVATE | JSCLASS_IMPLEMENTS_BARRIERS |
    JSCLASS_HAS_CACHED_PROTO(JSProto_Set),
    nullptr, // addProperty
    nullptr, // delProperty
    nullptr, // getProperty
    nullptr, // setProperty
    nullptr, // enumerate
    nullptr, // resolve
    nullptr, // convert
    finalize,
    nullptr, // call
    nullptr, // hasInstance
    nullptr, // construct
    mark
};

const JSPropertySpec SetObject::properties[] = {
    JS_PSG("size", size, 0),
    JS_PS_END
};

const JSFunctionSpec SetObject::methods[] = {
    JS_FN("has", has, 1, 0),
    JS_FN("add", add, 1, 0),
    JS_FN("delete", delete_, 1, 0),
    JS_FN("entries", entries, 0, 0),
    JS_FN("clear", clear, 0, 0),
    JS_SELF_HOSTED_FN("forEach", "SetForEach", 2, 0),
    JS_FS_END
};

JSObject*
SetObject::initClass(JSContext* cx, JSObject* obj)
{
    Rooted<GlobalObject*> global(cx, &obj->as<GlobalObject>());
    RootedObject proto(cx,
        InitClass(cx, global, &class_, JSProto_Set, construct, properties, methods));
    if (proto) {
        // Define the "values" method.
        JSFunction* fun = JS_DefineFunction(cx, proto, "values", values, 0, 0);
        if (!fun)
            return nullptr;

        // Define its aliases.
        RootedValue funval(cx, ObjectValue(*fun));
        if (!JS_DefineProperty(cx, proto, "keys", funval, 0))
            return nullptr;

        RootedId iteratorId(cx, SYMBOL_TO_JSID(cx->wellKnownSymbols().iterator));
        if (!JS_DefinePropertyById(cx, proto, iteratorId, funval, 0))
            return nullptr;
    }
    return proto;
}


bool
SetObject::keys(JSContext* cx, HandleObject obj, JS::AutoValueVector* keys)
{
    ValueSet* set = obj->as<SetObject>().getData();
    if (!set)
        return false;

    for (ValueSet::Range r = set->all(); !r.empty(); r.popFront()) {
        if (!keys->append(r.front().get()))
            return false;
    }

    return true;
}

bool
SetObject::add(JSContext* cx, HandleObject obj, HandleValue k)
{
    ValueSet* set = obj->as<SetObject>().getData();
    if (!set)
        return false;

    AutoHashableValueRooter key(cx);
    if (!key.setValue(cx, k))
        return false;

    if (!set->put(key)) {
        js_ReportOutOfMemory(cx);
        return false;
    }
    WriteBarrierPost(cx->runtime(), set, key.get());
    return true;
}

SetObject*
SetObject::create(JSContext* cx)
{
    SetObject* obj = NewBuiltinClassInstance<SetObject>(cx);
    if (!obj)
        return nullptr;

    ValueSet* set = cx->new_<ValueSet>(cx->runtime());
    if (!set || !set->init()) {
        js_delete(set);
        js_ReportOutOfMemory(cx);
        return nullptr;
    }
    obj->setPrivate(set);
    return obj;
}

void
SetObject::mark(JSTracer* trc, JSObject* obj)
{
    SetObject* setobj = static_cast<SetObject*>(obj);
    if (ValueSet* set = setobj->getData()) {
        for (ValueSet::Range r = set->all(); !r.empty(); r.popFront())
            MarkKey(r, r.front(), trc);
    }
}

void
SetObject::finalize(FreeOp* fop, JSObject* obj)
{
    SetObject* setobj = static_cast<SetObject*>(obj);
    if (ValueSet* set = setobj->getData())
        fop->delete_(set);
}

bool
SetObject::construct(JSContext* cx, unsigned argc, Value* vp)
{
    Rooted<SetObject*> obj(cx, SetObject::create(cx));
    if (!obj)
        return false;

    CallArgs args = CallArgsFromVp(argc, vp);

    // FIXME: bug 1083752
    if (!WarnIfNotConstructing(cx, args, "Set"))
        return false;

    if (!args.get(0).isNullOrUndefined()) {
        RootedValue adderVal(cx);
        if (!GetProperty(cx, obj, obj, cx->names().add, &adderVal))
            return false;

        if (!IsCallable(adderVal))
            return ReportIsNotFunction(cx, adderVal);

        bool isOriginalAdder = IsNativeFunction(adderVal, SetObject::add);
        RootedValue setVal(cx, ObjectValue(*obj));
        FastInvokeGuard fig(cx, adderVal);
        InvokeArgs& args2 = fig.args();

        RootedValue keyVal(cx);
        ForOfIterator iter(cx);
        if (!iter.init(args[0]))
            return false;
        AutoHashableValueRooter key(cx);
        ValueSet* set = obj->getData();
        while (true) {
            bool done;
            if (!iter.next(&keyVal, &done))
                return false;
            if (done)
                break;

            if (isOriginalAdder) {
                if (!key.setValue(cx, keyVal))
                    return false;
                if (!set->put(key)) {
                    js_ReportOutOfMemory(cx);
                    return false;
                }
                WriteBarrierPost(cx->runtime(), set, keyVal);
            } else {
                if (!args2.init(1))
                    return false;

                args2.setCallee(adderVal);
                args2.setThis(setVal);
                args2[0].set(keyVal);

                if (!fig.invoke(cx))
                    return false;
            }
        }
    }

    args.rval().setObject(*obj);
    return true;
}

bool
SetObject::is(HandleValue v)
{
    return v.isObject() && v.toObject().hasClass(&class_) && v.toObject().as<SetObject>().getPrivate();
}

ValueSet&
SetObject::extract(CallReceiver call)
{
    MOZ_ASSERT(call.thisv().isObject());
    MOZ_ASSERT(call.thisv().toObject().hasClass(&SetObject::class_));
    return *static_cast<SetObject&>(call.thisv().toObject()).getData();
}

bool
SetObject::size_impl(JSContext* cx, CallArgs args)
{
    MOZ_ASSERT(is(args.thisv()));

    ValueSet& set = extract(args);
    static_assert(sizeof(set.count()) <= sizeof(uint32_t),
                  "set count must be precisely representable as a JS number");
    args.rval().setNumber(set.count());
    return true;
}

bool
SetObject::size(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<SetObject::is, SetObject::size_impl>(cx, args);
}

bool
SetObject::has_impl(JSContext* cx, CallArgs args)
{
    MOZ_ASSERT(is(args.thisv()));

    ValueSet& set = extract(args);
    ARG0_KEY(cx, args, key);
    args.rval().setBoolean(set.has(key));
    return true;
}

bool
SetObject::has(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<SetObject::is, SetObject::has_impl>(cx, args);
}

bool
SetObject::add_impl(JSContext* cx, CallArgs args)
{
    MOZ_ASSERT(is(args.thisv()));

    ValueSet& set = extract(args);
    ARG0_KEY(cx, args, key);
    if (!set.put(key)) {
        js_ReportOutOfMemory(cx);
        return false;
    }
    WriteBarrierPost(cx->runtime(), &set, key.get());
    args.rval().set(args.thisv());
    return true;
}

bool
SetObject::add(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<SetObject::is, SetObject::add_impl>(cx, args);
}

bool
SetObject::delete_impl(JSContext* cx, CallArgs args)
{
    MOZ_ASSERT(is(args.thisv()));

    ValueSet& set = extract(args);
    ARG0_KEY(cx, args, key);
    bool found;
    if (!set.remove(key, &found)) {
        js_ReportOutOfMemory(cx);
        return false;
    }
    args.rval().setBoolean(found);
    return true;
}

bool
SetObject::delete_(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<SetObject::is, SetObject::delete_impl>(cx, args);
}

bool
SetObject::iterator_impl(JSContext* cx, CallArgs args, IteratorKind kind)
{
    Rooted<SetObject*> setobj(cx, &args.thisv().toObject().as<SetObject>());
    ValueSet& set = *setobj->getData();
    Rooted<JSObject*> iterobj(cx, SetIteratorObject::create(cx, setobj, &set, kind));
    if (!iterobj)
        return false;
    args.rval().setObject(*iterobj);
    return true;
}

bool
SetObject::values_impl(JSContext* cx, CallArgs args)
{
    return iterator_impl(cx, args, Values);
}

bool
SetObject::values(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod(cx, is, values_impl, args);
}

bool
SetObject::entries_impl(JSContext* cx, CallArgs args)
{
    return iterator_impl(cx, args, Entries);
}

bool
SetObject::entries(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod(cx, is, entries_impl, args);
}

bool
SetObject::clear_impl(JSContext* cx, CallArgs args)
{
    Rooted<SetObject*> setobj(cx, &args.thisv().toObject().as<SetObject>());
    if (!setobj->getData()->clear()) {
        js_ReportOutOfMemory(cx);
        return false;
    }
    args.rval().setUndefined();
    return true;
}

bool
SetObject::clear(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod(cx, is, clear_impl, args);
}

JSObject*
js_InitSetClass(JSContext* cx, HandleObject obj)
{
    return SetObject::initClass(cx, obj);
}

const JSFunctionSpec selfhosting_collection_iterator_methods[] = {
    JS_FN("std_Map_iterator_next", MapIteratorObject::next, 0, 0),
    JS_FN("std_Set_iterator_next", SetIteratorObject::next, 0, 0),
    JS_FS_END
};

bool
js::InitSelfHostingCollectionIteratorFunctions(JSContext* cx, HandleObject obj)
{
    return JS_DefineFunctions(cx, obj, selfhosting_collection_iterator_methods);
}

/*** JS public APIs **********************************************************/

JS_PUBLIC_API(JSObject*)
JS::NewMapObject(JSContext* cx)
{
    return MapObject::create(cx);
}

JS_PUBLIC_API(uint32_t)
JS::MapSize(JSContext* cx, HandleObject obj)
{
    CHECK_REQUEST(cx);
    return MapObject::size(cx, obj);
}

JS_PUBLIC_API(bool)
JS::MapGet(JSContext* cx, HandleObject obj,
           HandleValue key, MutableHandleValue rval)
{
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, key, rval);
    return MapObject::get(cx, obj, key, rval);
}

JS_PUBLIC_API(bool)
JS::MapHas(JSContext* cx, HandleObject obj, HandleValue key, bool* rval)
{
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, key);
    return MapObject::has(cx, obj, key, rval);
}

JS_PUBLIC_API(bool)
JS::MapSet(JSContext* cx, HandleObject obj,
           HandleValue key, HandleValue val)
{
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, key, val);
    return MapObject::set(cx, obj, key, val);
}

JS_PUBLIC_API(bool)
JS::MapClear(JSContext* cx, HandleObject obj)
{
    CHECK_REQUEST(cx);
    return MapObject::clear(cx, obj);
}

JS_PUBLIC_API(bool)
JS::MapKeys(JSContext* cx, HandleObject obj, MutableHandleValue rval)
{
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, rval);
    return MapObject::iterator(cx, MapObject::Keys, obj, rval);
}

JS_PUBLIC_API(bool)
JS::MapValues(JSContext* cx, HandleObject obj, MutableHandleValue rval)
{
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, rval);
    return MapObject::iterator(cx, MapObject::Values, obj, rval);
}

JS_PUBLIC_API(bool)
JS::MapEntries(JSContext* cx, HandleObject obj, MutableHandleValue rval)
{
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, rval);
    return MapObject::iterator(cx, MapObject::Entries, obj, rval);
}
