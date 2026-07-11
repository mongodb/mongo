// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/assert_util_core.h"
#include "mongo/util/modules.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <random>
#include <utility>

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * A uniform random bit generator based on XorShift.
 * Uses http://en.wikipedia.org/wiki/Xorshift
 */
class XorShift128 {
public:
    using result_type = uint32_t;

    static constexpr result_type min() {
        return std::numeric_limits<result_type>::lowest();
    }

    static constexpr result_type max() {
        return std::numeric_limits<result_type>::max();
    }

    explicit XorShift128(uint32_t seed) : _x{seed} {}

    result_type operator()() {
        uint32_t t = _x ^ (_x << 11);
        _x = _y;
        _y = _z;
        _z = _w;
        return _w = _w ^ (_w >> 19) ^ (t ^ (t >> 8));
    }

private:
    uint32_t _x;  // seed
    uint32_t _y = 362436069;
    uint32_t _z = 521288629;
    uint32_t _w = 88675123;
};

/** The SecureUrbg impls all produce the full range of uint64_t. */
class SecureUrbg {
public:
    using result_type = uint64_t;
    static constexpr result_type min() {
        return std::numeric_limits<result_type>::lowest();
    }
    static constexpr result_type max() {
        return std::numeric_limits<result_type>::max();
    }

    // Details including State vary by platform and are deferred to the .cpp file.
    SecureUrbg();
    ~SecureUrbg();
    result_type operator()();

private:
    class State;
    std::unique_ptr<State> _state;
};

// Provides mongo-traditional functions around a pluggable UniformRandomBitGenerator.
template <typename Urbg>
class [[MONGO_MOD_FILE_PRIVATE]] RandomBase {
public:
    using urbg_type = Urbg;

    RandomBase() : _urbg{} {}
    explicit RandomBase(urbg_type u) : _urbg{std::move(u)} {}

    /** The underlying generator */
    [[MONGO_MOD_PUBLIC]] urbg_type& urbg() {
        return _urbg;
    }

    /**
     * A random number in the range [0, 1).
     *
     * WARNING: May invoke slow floating-point library calls (e.g. __logl_finite) that are
     * software-emulated on some ARM64 platforms. Prefer trueWithProbability() rather than
     * comparing this to a probability.
     */
    [[MONGO_MOD_PUBLIC]] double nextCanonicalDouble() {
        return std::uniform_real_distribution<double>{0, 1}(_urbg);
    }

    /** A number uniformly distributed over all possible values. */
    [[MONGO_MOD_PUBLIC]] int32_t nextInt32() {
        return _nextAny<int32_t>();
    }

    /** A number uniformly distributed over all possible values. */
    [[MONGO_MOD_PUBLIC]] uint32_t nextUInt32() {
        return _nextAny<uint32_t>();
    }

    /** A number uniformly distributed over all possible values. */
    [[MONGO_MOD_PUBLIC]] int64_t nextInt64() {
        return _nextAny<int64_t>();
    }

    /** A number uniformly distributed over all possible values. */
    [[MONGO_MOD_PUBLIC]] uint64_t nextUInt64() {
        return _nextAny<uint64_t>();
    }

    /** A number in the half-open interval [0, max) */
    [[MONGO_MOD_PUBLIC]] int32_t nextInt32(int32_t max) {
        return std::uniform_int_distribution<int32_t>(0, max - 1)(_urbg);
    }

    /** A number in the half-open interval [0, max) */
    [[MONGO_MOD_PUBLIC]] uint32_t nextUInt32(uint32_t max) {
        return std::uniform_int_distribution<uint32_t>(0, max - 1)(_urbg);
    }

    /** A number in the half-open interval [0, max) */
    [[MONGO_MOD_PUBLIC]] int64_t nextInt64(int64_t max) {
        return std::uniform_int_distribution<int64_t>(0, max - 1)(_urbg);
    }

    /** A number in the half-open interval [0, max) */
    [[MONGO_MOD_PUBLIC]] uint64_t nextUInt64(uint64_t max) {
        return std::uniform_int_distribution<uint64_t>(0, max - 1)(_urbg);
    }

    /** Returns true with the given probability in [0, 1]. */
    [[MONGO_MOD_PUBLIC]] bool trueWithProbability(double probability) {
        dassert(0 <= probability && probability <= 1);
        return nextUInt32(std::numeric_limits<uint32_t>::max()) <
            uint32_t(probability * std::numeric_limits<uint32_t>::max());
    }

    /**
     A number uniformly distributed over all possible values that can be safely represented as
     double without loosing precision.
    */
    [[MONGO_MOD_PUBLIC]] int64_t nextInt64SafeDoubleRepresentable() {
        const int64_t maxRepresentableLimit =
            static_cast<int64_t>(std::ldexp(1, std::numeric_limits<double>::digits)) + 1;
        return nextInt64(maxRepresentableLimit);
    }

    /** Fill array `buf` with `n` random bytes. */
    [[MONGO_MOD_PUBLIC]] void fill(void* buf, size_t n) {
        const auto p = static_cast<uint8_t*>(buf);
        size_t written = 0;
        while (written < n) {
            uint64_t t = nextInt64();
            size_t w = std::min(n - written, sizeof(t));
            std::memcpy(p + written, &t, w);
            written += w;
        }
    }

private:
    template <typename T>
    T _nextAny() {
        using Limits = std::numeric_limits<T>;
        return std::uniform_int_distribution<T>(Limits::lowest(), Limits::max())(_urbg);
    }

    urbg_type _urbg;
};

/**
 * A Pseudorandom generator that's not cryptographically secure, but very fast and small.
 */
class PseudoRandom : public RandomBase<XorShift128> {
    using Base = RandomBase<XorShift128>;

public:
    explicit PseudoRandom(uint32_t seed) : Base{XorShift128{seed}} {}
    explicit PseudoRandom(int32_t seed) : PseudoRandom{static_cast<uint32_t>(seed)} {}
    explicit PseudoRandom(uint64_t seed)
        : PseudoRandom{static_cast<uint32_t>(seed ^ (seed >> 32))} {}
    explicit PseudoRandom(int64_t seed) : PseudoRandom{static_cast<uint64_t>(seed)} {}
};

/**
 * More secure random numbers
 * Suitable for nonce/crypto
 * Slower than PseudoRandom, so only use when really need
 */
class SecureRandom : public RandomBase<SecureUrbg> {
    using Base = RandomBase<SecureUrbg>;

public:
    using Base::Base;
};

}  // namespace mongo
