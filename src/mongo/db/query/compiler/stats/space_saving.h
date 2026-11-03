// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/compiler/stats/value_utils.h"

#include <cstddef>
#include <string>
#include <vector>

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
     */
    explicit SpaceSavingSketch(size_t k);

    // Copying would corrupt the copy's data structures.
    SpaceSavingSketch(const SpaceSavingSketch&) = delete;
    SpaceSavingSketch& operator=(const SpaceSavingSketch&) = delete;
    // A defaulted move would leave the moved-from sketch holding raw pointers into the pool
    // vectors the destination now owns, so using it would corrupt the destination's lists.
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

    // A counter. Lives in exactly one bucket's element queue; the most recently updated element is
    // at the head. Counters are reused on eviction, never reallocated.
    struct Element {
        SBEValue _value;
        size_t _err = 0;
        Bucket* _bucket = nullptr;
        Element* _prev = nullptr;  // Towards the head of the bucket.
        Element* _next = nullptr;  // Towards the tail of the bucket.
    };

    // A bucket: all counters sharing one estimate value. The tail of the minimum bucket is the one
    // evicted (the least recently updated element with the lowest count). Buckets are kept in a
    // doubly linked list ordered by count ascending. Unused buckets are chained through '_next' on
    // '_freeBuckets'.
    struct Bucket {
        size_t _count = 0;
        Element* _head = nullptr;
        Element* _tail = nullptr;
        Bucket* _prev = nullptr;
        Bucket* _next = nullptr;
    };

    // Pushes 'element' at the head of the bucket for 'count'. 'before' must be the bucket that
    // follows the position where 'count' belongs (nullptr for the end of the list): it is reused if
    // it has the same count, otherwise a bucket from the pool is linked in front of it.
    void _insertIntoBucketBefore(Element* element, Bucket* before, size_t count);

    // Removes 'element' from its bucket, releasing the bucket if it becomes empty.
    void _removeFromBucket(Element* element);

    size_t _k;
    // Both vectors are sized once in the constructor and never reallocate, so the raw pointers
    // above stay valid for the lifetime of the sketch.
    std::vector<Element> _counters;
    std::vector<Bucket> _bucketPool;
    Bucket* _freeBuckets = nullptr;
    Bucket* _minBucket = nullptr;
    Bucket* _maxBucket = nullptr;
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
     * with exact counts.
     */
    static MostCommonValues make(const std::vector<SBEValue>& values, size_t k);

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
