/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jsfixedsizehash_h_
#define jsfixedsizehash_h_

#include "ds/LifoAlloc.h"

namespace js {

/*
 * Class representing a hash set with fixed capacity, with newer entries
 * evicting older entries. Each entry has several hashes and can be stored in
 * different buckets, with the choice of which to evict on insertion being
 * managed via LRU. For tables with a relatively small size, using different
 * hashes increases utilization and makes it less likely that entries will keep
 * evicting each other due to wanting to use the same bucket.
 *
 * T indicates the type of hash elements, HashPolicy must have the following
 * contents:
 *
 * Lookup - As for HashMap / HashSet.
 *
 * bool match(T, Lookup) - As for HashMap / HashSet.
 *
 * NumHashes - Number of different hashes generated for each entry.
 *
 * void hash(Lookup, HashNumber[NumHashes]) - Compute all hashes for an entry.
 *
 * void clear(T*) - Clear an entry, such that isCleared() holds afterwards.
 *
 * bool isCleared(T) - Test whether an entry has been cleared.
 */
template <class T, class HashPolicy, size_t Capacity>
class FixedSizeHashSet
{
    T entries[Capacity];
    uint32_t lastOperations[Capacity];
    uint32_t numOperations;

    static const size_t NumHashes = HashPolicy::NumHashes;

    static_assert(Capacity > 0, "an empty fixed-size hash set is meaningless");

  public:
    typedef typename HashPolicy::Lookup Lookup;

    FixedSizeHashSet()
      : entries(), lastOperations(), numOperations(0)
    {
        MOZ_ASSERT(HashPolicy::isCleared(entries[0]));
    }

    bool lookup(const Lookup& lookup, T* pentry)
    {
        size_t bucket;
        if (lookupReference(lookup, &bucket)) {
            *pentry = entries[bucket];
            lastOperations[bucket] = numOperations++;
            return true;
        }
        return false;
    }

    void insert(const Lookup& lookup, const T& entry)
    {
        size_t buckets[NumHashes];
        getBuckets(lookup, buckets);

        size_t min = buckets[0];
        for (size_t i = 0; i < NumHashes; i++) {
            const T& entry = entries[buckets[i]];
            if (HashPolicy::isCleared(entry)) {
                entries[buckets[i]] = entry;
                lastOperations[buckets[i]] = numOperations++;
                return;
            }
            if (i && lastOperations[min] > lastOperations[buckets[i]])
                min = buckets[i];
        }

        entries[min] = entry;
        lastOperations[min] = numOperations++;
    }

    template <typename S>
    void remove(const S& s)
    {
        size_t bucket;
        if (lookupReference(s, &bucket))
            HashPolicy::clear(&entries[bucket]);
    }

  private:
    template <typename S>
    bool lookupReference(const S& s, size_t* pbucket)
    {
        size_t buckets[NumHashes];
        getBuckets(s, buckets);

        for (size_t i = 0; i < NumHashes; i++) {
            const T& entry = entries[buckets[i]];
            if (!HashPolicy::isCleared(entry) && HashPolicy::match(entry, s)) {
                *pbucket = buckets[i];
                return true;
            }
        }

        return false;
    }

    template <typename S>
    void getBuckets(const S& s, size_t buckets[NumHashes])
    {
        HashNumber hashes[NumHashes];
        HashPolicy::hash(s, hashes);

        for (size_t i = 0; i < NumHashes; i++)
            buckets[i] = hashes[i] % Capacity;
    }
};

}  /* namespace js */

#endif /* jsfixedsizehash_h_ */
