// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace mongo::ce {

/**
 * HyperLogLog is a probabilistic data structure for estimating the number of distinct values in a
 * multiset using a fixed amount of memory, see Flajolet et al. (2007), "HyperLogLog: the analysis
 * of a near-optimal cardinality estimation algorithm".
 *
 * The sketch maintains 2^precision single-byte registers (e.g. 16KB for precision 14). Values are
 * added by their 64-bit hash: the top 'precision' bits select a register and the remaining bits
 * contribute the position of their leftmost 1-bit, of which each register keeps the maximum. The
 * cardinality estimate is derived from the harmonic mean of the register values.
 *
 * The relative standard error of the estimate is ~1.04/sqrt(2^precision), e.g. ~0.81% for
 * precision 14. That is the standard deviation of the estimator, not a strict bound. Estimates of
 * cardinalities small relative to the register count use linear counting on the number of empty
 * registers instead, which makes them near-exact; the switchover region around 2.5 * 2^precision
 * exhibits a slightly elevated bias (see Heule et al. (2013), "HyperLogLog in practice", section
 * 5.2, whose empirical bias correction we do not implement).
 *
 * WARNING - the sketch is only as correct as the hashes fed into it, and it has no way to detect
 * misuse. The caller must uphold all of the following:
 *  - Every hash that ever reaches one sketch - including sketches it is merged with, across
 *    processes and across (de)serialization - must come from ONE hash function with ONE seed. Do
 *    not hash values yourself; use the single shared producer for the use case (for NDV
 *    estimation: hashValueForNdv() in ndv_hashing.h). Mixing hash functions silently corrupts
 *    estimates without any error.
 *  - Values that are considered equal must hash identically.
 *  - The hash function must distribute all 64 bits uniformly; a weak hash skews the estimate.
 * Distinct values may still collide, but with 64-bit hashes collisions are negligible for any
 * practically reachable cardinality, so no correction for them ("large range correction") is
 * applied.
 *
 * This class is not thread-safe.
 */
class HyperLogLog {
public:
    // Bounds of the supported precision range. These are invariants of the algorithm rather than
    // tunables: the paper's bias-correction constants and error analysis assume at least 16
    // registers (precision 4), and 18 caps a sketch at a sane 256KB - already far more accurate
    // (~0.2% standard error) than this class's use cases require. Which precision to use within
    // the range is the caller's choice.
    static constexpr size_t kMinPrecision = 4;
    static constexpr size_t kMaxPrecision = 18;

    using Registers = std::span<const uint8_t>;

    /**
     * Creates an empty sketch with 2^precision registers. Returns BadValue if 'precision' is not
     * in [kMinPrecision, kMaxPrecision].
     */
    static StatusWith<HyperLogLog> create(size_t precision);

    /**
     * Recreates a sketch from previously serialized state; the inverse of 'registers()'. Returns
     * BadValue if 'precision' is out of range, 'registers' does not hold exactly 2^precision
     * values, or any register exceeds 64 - precision + 1, so untrusted (e.g. persisted) state is
     * validated rather than trusted.
     */
    static StatusWith<HyperLogLog> create(size_t precision, Registers registers);

    /**
     * Adds a value, represented by its 64-bit hash, to the multiset. Duplicate-insensitive:
     * adding a hash that has been added before never changes the state. The hash must satisfy the
     * contract in the class comment; in particular it must come from the same hash function as
     * every other hash this sketch (or any sketch merged with it) observes.
     */
    void addHash(uint64_t hash);

    /**
     * Returns an estimate of the number of distinct hashes added so far.
     */
    double estimate() const;

    /**
     * Merges 'other' into this sketch by taking the register-wise maximum. The result is
     * indistinguishable from a sketch that observed the inputs of both. Both sketches must have
     * been created with the same precision, and their inputs must have been hashed with the same
     * hash function (the latter cannot be enforced here).
     */
    void merge(const HyperLogLog& other);

    size_t precision() const {
        return _precision;
    }

    /**
     * Read-only view of the register array, of size 2^precision. Exposed for tests and for
     * serializing the sketch state; a sketch can be recreated from it with 'create()'.
     */
    Registers registers() const {
        return _registers;
    }

    /**
     * Approximate memory footprint of this sketch in bytes.
     */
    uint64_t getApproximateSize() const {
        return sizeof(*this) + _registers.capacity();
    }

private:
    /**
     * Creates an empty sketch. Performs no validation; only reachable from 'create()', which has
     * already validated 'precision'.
     */
    explicit HyperLogLog(size_t precision);

    /**
     * Copies register state. Performs no validation; only reachable from 'create()', which has
     * already validated the state.
     */
    HyperLogLog(size_t precision, Registers registers);

    // One byte per register; register i holds the maximum rank (position of the leftmost 1-bit,
    // 1-based) observed among the hashes routed to it, or 0 if it has not been touched yet. The
    // array size is 2^_precision.
    std::vector<uint8_t> _registers;

    // Number of hash bits used to select the register; determines the register count and thereby
    // the memory/accuracy trade-off. In [kMinPrecision, kMaxPrecision].
    size_t _precision;
};

}  // namespace mongo::ce
