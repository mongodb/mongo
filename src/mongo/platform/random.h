/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <random>

namespace mongo {

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
class RandomBase {
public:
    using urbg_type = Urbg;

    RandomBase() : _urbg{} {}
    explicit RandomBase(urbg_type u) : _urbg{std::move(u)} {}

    /** The underlying generator */
    urbg_type& urbg() {
        return _urbg;
    }

    /** A random number in the range [0, 1). */
    double nextCanonicalDouble() {
        return std::uniform_real_distribution<double>{0, 1}(_urbg);
    }

    /** A number uniformly distributed over all possible values. */
    int32_t nextInt32() {
        return _nextAny<int32_t>();
    }

    /** A number uniformly distributed over all possible values. */
    int64_t nextInt64() {
        return _nextAny<int64_t>();
    }

    /** A number in the half-open interval [0, max) */
    int32_t nextInt32(int32_t max) {
        return std::uniform_int_distribution<int32_t>(0, max - 1)(_urbg);
    }

    /** A number in the half-open interval [0, max) */
    int64_t nextInt64(int64_t max) {
        return std::uniform_int_distribution<int64_t>(0, max - 1)(_urbg);
    }

    /** Fill array `buf` with `n` random bytes. */
    void fill(void* buf, size_t n) {
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
