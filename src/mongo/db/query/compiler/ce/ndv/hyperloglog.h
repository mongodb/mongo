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
 * Estimates the number of distinct values in a multiset with fixed memory (Flajolet et al. 2007).
 * 2^precision single-byte registers; a value's 64-bit hash picks a register with its top bits and
 * the register keeps the max leading-zero rank of the rest. The estimate is the bias-corrected
 * harmonic mean of the registers.
 *
 * Standard error is ~1.04/sqrt(2^precision), so ~0.81% at precision 14 (a standard deviation,
 * not a bound). Small cardinalities fall back to linear counting and are near-exact; around
 * 2.5 * 2^precision there is a slightly elevated bias (Heule et al. 2013, section 5.2; we skip
 * their empirical correction).
 *
 * WARNING: the sketch can't detect bad hashes. One hash function, one seed, for every hash a
 * sketch ever sees, including anything merged into it, across processes and (de)serialization.
 * Don't hash values yourself; use the shared producer (hashValueForNdv() for NDV). Equal values
 * must hash identically and all 64 bits must be uniform. Collisions between distinct values are
 * negligible at 64 bits, so there is no large range correction.
 *
 * Not thread-safe.
 */
class HyperLogLog {
public:
    // Algorithm limits, not tunables: the paper's constants assume at least 16 registers, and
    // 18 caps a sketch at 256KB (~0.2% error, far beyond our needs). Callers pick a precision in
    // this range.
    static constexpr size_t kMinPrecision = 4;
    static constexpr size_t kMaxPrecision = 18;

    using Registers = std::span<const uint8_t>;

    /**
     * Creates an empty sketch with 2^precision registers. Returns BadValue if 'precision' is not
     * in [kMinPrecision, kMaxPrecision].
     */
    static StatusWith<HyperLogLog> create(size_t precision);

    /**
     * Recreates a sketch from serialized state; the inverse of 'registers()'. Returns BadValue
     * for a bad precision, wrong register count, or a register above 64 - precision + 1, so
     * persisted state is validated, not trusted.
     */
    static StatusWith<HyperLogLog> create(size_t precision, Registers registers);

    /**
     * Adds a value by its 64-bit hash (see the class comment for the hash contract). Adding the
     * same hash again never changes the state.
     */
    void addHash(uint64_t hash);

    /**
     * Returns an estimate of the number of distinct hashes added so far.
     */
    double estimate() const;

    /**
     * Register-wise max: afterwards this sketch is as if it had also seen 'other''s inputs.
     * Requires the same precision and the same hash function (the latter can't be enforced).
     */
    void merge(const HyperLogLog& other);

    size_t precision() const {
        return _precision;
    }

    /**
     * Read-only view of the 2^precision registers, for serialization and tests; 'create()' is
     * the inverse.
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
     * No validation; 'create()' has already done it.
     */
    explicit HyperLogLog(size_t precision);

    /**
     * Copies validated register state; only reachable from 'create()'.
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
