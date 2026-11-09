// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/compiler/stats/value_utils.h"

#include <cstddef>
#include <string>
#include <vector>

#include <boost/intrusive/list.hpp>
#include <boost/intrusive/list_hook.hpp>

namespace mongo::stats {

/**
 * This file implements the Space-Saving algorithm: a single-pass, fixed-memory algorithm for
 * estimating the frequencies of the values in a data stream and finding its top-k most
 * frequent values. Based on:
 *
 *     Ahmed Metwally, Divyakant Agrawal, and Amr El Abbadi: "Efficient Computation of
 *     Frequent and Top-k Elements in Data Streams". ICDT 2005, pp. 398-412. Springer, 2005.
 *     https://doi.org/10.1007/978-3-540-30570-5_27
 */

/**
 * One entry of a Space-Saving result: a most common value (MCV), its estimated frequency,
 * and the error term that bounds the overestimation. The true frequency of
 * the value is guaranteed to lie in [estFreq - err, estFreq].
 */
struct MCVEntry {
    MCVEntry(SBEValue value, size_t estFreq, size_t err)
        : value(std::move(value)), estFreq(estFreq), err(err) {}

    std::string toString() const;

    SBEValue value;
    size_t estFreq;
    size_t err;
};

/**
 * Minimum number of counters a SpaceSavingSketch can hold; smaller arguments are
 * clamped up to this.
 */
inline constexpr std::size_t kMinNumCounters = 10;

/**
 * Estimates the top-k most frequent values in a stream of values with fixed memory,
 * using the paper's data structure in full: a hash table
 * for O(1) membership plus an intrusive doubly linked list of count buckets (front = minimum
 * count), so that incrementing a counter and evicting the minimum are O(1) amortized.
 *
 * Each monitored value has a counter with an estimate and an error term. When a new value
 * arrives and all k counters are used, the counter with the minimum estimate is evicted and
 * reused: the newcomer's estimate is set to the evicted count plus 1 and its error term to
 * the evicted count, bounding the newcomer's true count between 1 and the evicted count + 1.
 *
 * Not thread-safe.
 */
class SpaceSavingSketch {
public:
    /**
     * Creates a sketch holding at most 'k' counters. Values of 'k' below kMinNumCounters
     * are clamped up to it, so the sketch always holds at least kMinNumCounters counters.
     *
     * If 'collator' is non-null, strings are compared and hashed under it, so values that are
     * equal under the collation share one counter. The reported value is then an arbitrary
     * representative of its equivalence class (the one that arrived when the counter was created
     * or last reused). 'collator' is not owned and must outlive the sketch.
     */
    SpaceSavingSketch(size_t k, const CollatorInterface* collator);

    // Copying would corrupt the copy's data structures.
    SpaceSavingSketch(const SpaceSavingSketch&) = delete;
    SpaceSavingSketch& operator=(const SpaceSavingSketch&) = delete;
    // A moved-from sketch would be unusable: its pools are empty but its counters are not.
    SpaceSavingSketch(SpaceSavingSketch&&) = delete;
    SpaceSavingSketch& operator=(SpaceSavingSketch&&) = delete;

    /**
     * Adds one stream element to the sketch. Adding a value that is already monitored only
     * increments its counter.
     */
    void increment(const SBEValue& value);

    /**
     * Returns the monitored most common values (MCVs) sorted by estimated frequency
     * descending (ties broken by BSON value order ascending). If the sketch never evicted a
     * counter, all estimates are exact.
     */
    std::vector<MCVEntry> getTopK() const;

    size_t numCounters() const {
        return _counters.size();
    }

private:
    struct Bucket;

    using Hook = boost::intrusive::list_member_hook<
        boost::intrusive::link_mode<boost::intrusive::auto_unlink>>;

    // A counter. Lives in exactly one bucket's element list; the most recently updated element is
    // at the front. Counters are reused on eviction, never reallocated.
    struct Element {
        SBEValue _value;
        size_t _err = 0;
        Bucket* _bucket = nullptr;
        Hook _hook;
    };
    using ElementList =
        boost::intrusive::list<Element,
                               boost::intrusive::member_hook<Element, Hook, &Element::_hook>,
                               boost::intrusive::constant_time_size<false>>;

    // A bucket: all counters sharing one estimate value. The back of the minimum bucket's element
    // list is the one evicted (the least recently updated element with the lowest count). A bucket
    // is linked either into '_buckets' (ordered by count ascending) or into '_freeBuckets', never
    // both, so one hook serves both lists.
    struct Bucket {
        size_t _count = 0;
        ElementList _elements;
        Hook _hook;
    };
    using BucketList =
        boost::intrusive::list<Bucket,
                               boost::intrusive::member_hook<Bucket, Hook, &Bucket::_hook>,
                               boost::intrusive::constant_time_size<false>>;

    // Pushes 'element' at the front of the bucket for 'count'. 'before' must be the bucket that
    // follows the position where 'count' belongs (end() for the end of the list): it is reused if
    // it has the same count, otherwise a bucket from the pool is linked in front of it.
    void _insertIntoBucketBefore(Element* element, BucketList::iterator before, size_t count);

    // Removes 'element' from its bucket, releasing the bucket if it becomes empty.
    void _removeFromBucket(Element* element);

    size_t _k;
    const CollatorInterface* _collator;
    // Both vectors are sized once in the constructor and never reallocate, so the pointers held by
    // the lists and the hash table stay valid.
    std::vector<Element> _counters;
    std::vector<Bucket> _bucketPool;
    BucketList _freeBuckets;
    BucketList _buckets;  // Front = minimum count, back = maximum count.
    // Keys are non-owning views of the monitored Element's '_value'; an entry must be erased
    // before that value is overwritten.
    sbe::value::ValueMapType<Element*> _table;
};

/**
 * Result of a Space-Saving top-k computation: the most common values (MCVs) with their
 * estimated frequencies and error terms.
 */
struct MostCommonValues {
    /**
     * Given an array of values (treated as a stream in array order), compute the top-k most
     * frequent values with the Space-Saving algorithm. Values of 'k' below kMinNumCounters are
     * clamped up to it. If k is at least the number of distinct values, all values are returned
     * with exact counts. If 'collator' is non-null, values are compared under it (see
     * SpaceSavingSketch).
     */
    static MostCommonValues make(const std::vector<SBEValue>& values,
                                 size_t k,
                                 const CollatorInterface* collator = nullptr);

    /**
     * Returns the sum of the entries' guaranteed lower bounds (estFreq - err).
     */
    size_t getCumulativeLowBoundFrequency() const;

    std::string toString() const;

    /**
     * Find the prefix of the sorted entries such that the estimate lower bound (estFreq - err) of
     * each entry in the prefix strictly exceeds the overestimate estFreq of the entry after it.
     * Returns the number of entries in the guaranteed top-k prefix. Requires _entries to be sorted
     * as produced by getTopK().
     */
    size_t getGuaranteedTopK() const;

    std::vector<MCVEntry> _entries;

    // Number of elements in the input stream; used to compute the cumulative
    // frequencies as a percentage.
    size_t _inputSize = 0;
};

}  // namespace mongo::stats
