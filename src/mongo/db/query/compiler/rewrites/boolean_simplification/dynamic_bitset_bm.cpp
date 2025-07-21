/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/util/dynamic_bitset.h"

#include <bitset>

#include <benchmark/benchmark.h>
#include <boost/dynamic_bitset.hpp>

namespace mongo::boolean_simplification {

/**
 * The benchmark compares operations of different implementation of bitsets:
 * - DynamicBitset
 * - boost::dynamic_bitset
 * - std::bitset
 */

void benchOp(benchmark::State& state, auto op, auto&&... a) {
    for (auto _ : state) {
        (benchmark::DoNotOptimize(&a), ...);
        auto&& r = op(a...);
        benchmark::DoNotOptimize(&r);
    }
}

/**
 * Logical AND of DynamicBitset: a & b.
 */
template <size_t NumberOfBlocks, size_t BitsetSize>
void implMongoBitset_And(benchmark::State& state) {
    DynamicBitset<size_t, NumberOfBlocks> lhs(BitsetSize);
    DynamicBitset<size_t, NumberOfBlocks> rhs(BitsetSize);
    benchOp(state, [](auto&& a, auto&& b) { return a & b; }, lhs, rhs);
}

/**
 * Logical AND of boost::dynamic_bitset: a & b.
 */
template <size_t BitsetSize>
void implBoostBitset_And(benchmark::State& state) {
    boost::dynamic_bitset<size_t> lhs(BitsetSize);
    boost::dynamic_bitset<size_t> rhs(BitsetSize);
    benchOp(state, [](auto&& a, auto&& b) { return a & b; }, lhs, rhs);
}

/**
 * Logical AND of std::bitset: a & b.
 */
template <size_t BitsetSize>
void implStdBitset_And(benchmark::State& state) {
    std::bitset<BitsetSize> lhs;
    std::bitset<BitsetSize> rhs;
    benchOp(state, [](auto&& a, auto&& b) { return a & b; }, lhs, rhs);
}

template <typename intType>
void implInt_And(benchmark::State& state) {
    intType lhs{0};
    intType rhs{0};
    benchOp(state, [](auto&& a, auto&& b) { return a & b; }, lhs, rhs);
}

template <size_t NumberOfBlocks, size_t BitsetSize>
void implMongoBitset_AndEq(benchmark::State& state) {
    DynamicBitset<size_t, NumberOfBlocks> lhs(BitsetSize);
    DynamicBitset<size_t, NumberOfBlocks> rhs(BitsetSize);
    benchOp(state, [](auto&& a, auto&& b) -> decltype(auto) { return a &= b; }, lhs, rhs);
}

template <size_t BitsetSize>
void implStdBitset_AndEq(benchmark::State& state) {
    std::bitset<BitsetSize> lhs;
    std::bitset<BitsetSize> rhs;
    benchOp(state, [](auto&& a, auto&& b) -> decltype(auto) { return a &= b; }, lhs, rhs);
}

/**
 * Count of set bits of boost::dynamic_bitset.
 */
template <size_t BitsetSize>
void implBoostBitset_Count(benchmark::State& state) {
    boost::dynamic_bitset<size_t> bitset(BitsetSize);
    benchOp(state, [](auto&& a) { return a.count(); }, bitset);
}

/**
 * Count of set bits of std::bitset.
 */
template <size_t BitsetSize>
void implStdBitset_Count(benchmark::State& state) {
    std::bitset<BitsetSize> bitset;
    benchOp(state, [](auto&& a) { return a.count(); }, bitset);
}

template <size_t NumberOfBlocks, size_t BitsetSize>
void implMongoBitset_Count(benchmark::State& state) {
    DynamicBitset<size_t, NumberOfBlocks> bitset{BitsetSize};
    benchOp(state, [](auto&& a) { return a.count(); }, bitset);
}

/**
 * Equality and logical AND of boost::dynamic_bitset: a == (b & c).
 */
template <size_t BitsetSize>
void implBoostBitset_EqualityAnd(benchmark::State& state) {
    boost::dynamic_bitset<size_t> lhs(BitsetSize);
    boost::dynamic_bitset<size_t> rhs(BitsetSize);
    boost::dynamic_bitset<size_t> eq{BitsetSize};
    benchOp(state, [](auto&& a, auto&& b, auto&& eq) { return eq == (a & b); }, lhs, rhs, eq);
}

/**
 * Equality and logical AND of std::bitset: a == (b & c).
 */
template <size_t BitsetSize>
void implStdBitset_EqualityAnd(benchmark::State& state) {
    std::bitset<BitsetSize> lhs{11};
    std::bitset<BitsetSize> rhs{17};
    std::bitset<BitsetSize> eq{23};
    benchOp(state, [](auto&& a, auto&& b, auto&& eq) { return eq == (a & b); }, lhs, rhs, eq);
}

template <size_t NumberOfBlocks, size_t BitsetSize>
void implMongoBitset_EqualityAnd(benchmark::State& state) {
    DynamicBitset<size_t, NumberOfBlocks> lhs(BitsetSize);
    DynamicBitset<size_t, NumberOfBlocks> rhs(BitsetSize);
    DynamicBitset<size_t, NumberOfBlocks> eq(BitsetSize);
    benchOp(state, [](auto&& a, auto&& b, auto&& eq) { return eq == (a & b); }, lhs, rhs, eq);
}

template <size_t BitsetSize>
void implBoostBitset_CountNumberOfSetBits(benchmark::State& state) {
    boost::dynamic_bitset<size_t> bitset{BitsetSize};
    for (size_t i = 7; i < bitset.size(); i += 7) {
        bitset.set(i, true);
    }

    size_t count = 0;
    for (auto _ : state) {
        for (auto index = bitset.find_first(); index < bitset.size();
             index = bitset.find_next(index)) {
            benchmark::DoNotOptimize(count += 1);
        }
    }
}

template <size_t BitsetSize>
void implBoostBitset_CountNumberOfEmptySetBits(benchmark::State& state) {
    boost::dynamic_bitset<size_t> bitset{BitsetSize};

    size_t count = 0;
    for (auto _ : state) {
        for (auto index = bitset.find_first(); index < bitset.size();
             index = bitset.find_next(index)) {
            benchmark::DoNotOptimize(count += 1);
        }
    }
}

template <size_t BitsetSize>
void implStdBitset_CountNumberOfSetBits(benchmark::State& state) {
    std::bitset<BitsetSize> bitset{};
    for (size_t i = 7; i < bitset.size(); i += 7) {
        bitset.set(i, true);
    }

    size_t count = 0;
    for (auto _ : state) {
        for (size_t i = 0; i < bitset.size(); ++i) {
            if (bitset[i]) {
                benchmark::DoNotOptimize(count += 1);
            }
        }
    }
}

template <size_t BitsetSize>
void implStdBitset_CountNumberOfEmptySetBits(benchmark::State& state) {
    std::bitset<BitsetSize> bitset{};

    size_t count = 0;
    for (auto _ : state) {
        for (size_t i = 0; i < bitset.size(); ++i) {
            if (bitset[i]) {
                benchmark::DoNotOptimize(count += 1);
            }
        }
    }
}

template <size_t BitsetSize>
void implMongoBitset_CountNumberOfSetBits(benchmark::State& state) {
    DynamicBitset<size_t, 1> bitset(BitsetSize);
    for (size_t i = 7; i < bitset.size(); i += 7) {
        bitset.set(i, true);
    }

    size_t count = 0;
    for (auto _ : state) {
        for (auto index = bitset.findFirst(); index < bitset.size();
             index = bitset.findNext(index)) {
            benchmark::DoNotOptimize(count += 1);
        }
    }
}

template <size_t BitsetSize>
void implMongoBitset_CountNumberOfEmptySetBits(benchmark::State& state) {
    DynamicBitset<size_t, 1> bitset(BitsetSize);

    size_t count = 0;
    for (auto _ : state) {
        for (auto index = bitset.findFirst(); index < bitset.size();
             index = bitset.findNext(index)) {
            benchmark::DoNotOptimize(count += 1);
        }
    }
}

struct MintermStdBitset {
    void set(size_t bitIndex, bool value) {
        mask.set(bitIndex, true);
        predicates.set(bitIndex, value);
    }

    bool canAbsorb(const MintermStdBitset& other) const {
        return mask == (mask & other.mask) && predicates == (mask & other.predicates);
    }

    std::bitset<64> predicates{};
    std::bitset<64> mask{};
};

struct MintermMongoBitset {
    MintermMongoBitset() : predicates(64), mask(64) {}

    void set(size_t bitIndex, bool value) {
        mask.set(bitIndex, true);
        predicates.set(bitIndex, value);
    }

    MONGO_COMPILER_ALWAYS_INLINE bool canAbsorb(const MintermMongoBitset& other) const {
        return mask.isSubsetOf(other.mask) && predicates.isEqualToMasked(other.predicates, mask);
    }

    DynamicBitset<size_t, 1> predicates;
    DynamicBitset<size_t, 1> mask;
};

struct MintermBoostBitset {
    MintermBoostBitset() : predicates(64), mask(64) {}

    void set(size_t bitIndex, bool value) {
        mask.set(bitIndex, true);
        predicates.set(bitIndex, value);
    }

    MONGO_COMPILER_ALWAYS_INLINE bool canAbsorb(const MintermBoostBitset& other) const {
        return mask.is_subset_of(other.mask) && predicates == (mask & other.predicates);
    }

    boost::dynamic_bitset<size_t> predicates;
    boost::dynamic_bitset<size_t> mask;
};

/**
 * Fake implementation of Maxterm::removeRedundancies without side effects so it can be reliably
 * benchmarked.
 */
template <typename BenchMinterm>
void removeRedundanciesBench(std::vector<BenchMinterm>& minterms) {
    std::sort(
        minterms.begin(), minterms.end(), [](const BenchMinterm& lhs, const BenchMinterm& rhs) {
            return lhs.mask.count() < rhs.mask.count();
        });

    std::vector<BenchMinterm> newMinterms{};
    newMinterms.reserve(minterms.size());

    for (auto&& minterm : minterms) {
        bool absorbed = false;
        for (const auto& seenMinterm : newMinterms) {
            if (seenMinterm.canAbsorb(minterm)) {
                // Keep absorbed false to make sure that the original list of minterms won't be
                // changed.
                absorbed = false;
                break;
            }
        }
        if (!absorbed) {
            newMinterms.emplace_back(std::move(minterm));
        }
    }

    minterms.swap(newMinterms);
}

template <typename BenchMinterm>
void implBitset_removeRedundancies(benchmark::State& state) {
    static constexpr size_t kNumberOfBits = 64;
    const size_t numMinterms = static_cast<size_t>(state.range());

    std::vector<BenchMinterm> minterms{};
    minterms.resize(numMinterms);
    for (size_t i = 0; i < numMinterms; ++i) {
        minterms[i].set(i % kNumberOfBits, true);
        minterms[i].set((i * 46533 + 11) % kNumberOfBits, false);
        minterms[i].set((i * 542 + 7915) % (kNumberOfBits - 11), false);
    }

    for (auto _ : state) {
        removeRedundanciesBench(minterms);
    }
}

void implMongoBitset_Any(benchmark::State& state) {
    DynamicBitset<size_t, 1> bitset(64);
    bitset.set(10);

    for (auto _ : state) {
        benchmark::DoNotOptimize(bitset.any());
    }
}

void implMongoBitset_All(benchmark::State& state) {
    DynamicBitset<size_t, 1> bitset(64);
    bitset.set(10);

    for (auto _ : state) {
        benchmark::DoNotOptimize(bitset.any());
    }
}


BENCHMARK_TEMPLATE(implInt_And, uint64_t);
BENCHMARK_TEMPLATE(implStdBitset_And, 64);
BENCHMARK_TEMPLATE(implMongoBitset_And, 1, 64);
BENCHMARK_TEMPLATE(implMongoBitset_And, 2, 64);
BENCHMARK_TEMPLATE(implBoostBitset_And, 64);
BENCHMARK_TEMPLATE(implBoostBitset_And, 128);
BENCHMARK_TEMPLATE(implStdBitset_And, 128);
BENCHMARK_TEMPLATE(implMongoBitset_And, 1, 128);
BENCHMARK_TEMPLATE(implMongoBitset_And, 2, 128);
BENCHMARK_TEMPLATE(implStdBitset_And, 256);
BENCHMARK_TEMPLATE(implMongoBitset_And, 1, 256);
BENCHMARK_TEMPLATE(implMongoBitset_And, 2, 256);
BENCHMARK_TEMPLATE(implMongoBitset_And, 3, 256);
BENCHMARK_TEMPLATE(implMongoBitset_And, 4, 256);  // This fits
BENCHMARK_TEMPLATE(implBoostBitset_And, 256);
BENCHMARK_TEMPLATE(implStdBitset_And, 512);
BENCHMARK_TEMPLATE(implMongoBitset_And, 1, 512);
BENCHMARK_TEMPLATE(implMongoBitset_And, 2, 512);
BENCHMARK_TEMPLATE(implBoostBitset_And, 512);

BENCHMARK_TEMPLATE(implStdBitset_AndEq, 64);
BENCHMARK_TEMPLATE(implMongoBitset_AndEq, 1, 64);
BENCHMARK_TEMPLATE(implMongoBitset_AndEq, 2, 64);

BENCHMARK_TEMPLATE(implBoostBitset_Count, 64);
BENCHMARK_TEMPLATE(implStdBitset_Count, 64);
BENCHMARK_TEMPLATE(implMongoBitset_Count, 1, 64);
BENCHMARK_TEMPLATE(implMongoBitset_Count, 2, 64);
BENCHMARK_TEMPLATE(implBoostBitset_Count, 128);
BENCHMARK_TEMPLATE(implStdBitset_Count, 128);
BENCHMARK_TEMPLATE(implMongoBitset_Count, 1, 128);
BENCHMARK_TEMPLATE(implMongoBitset_Count, 2, 128);
BENCHMARK_TEMPLATE(implBoostBitset_Count, 256);
BENCHMARK_TEMPLATE(implStdBitset_Count, 256);
BENCHMARK_TEMPLATE(implMongoBitset_Count, 1, 256);
BENCHMARK_TEMPLATE(implMongoBitset_Count, 2, 256);
BENCHMARK_TEMPLATE(implBoostBitset_Count, 512);
BENCHMARK_TEMPLATE(implStdBitset_Count, 512);
BENCHMARK_TEMPLATE(implMongoBitset_Count, 1, 512);
BENCHMARK_TEMPLATE(implMongoBitset_Count, 2, 512);

BENCHMARK_TEMPLATE(implBoostBitset_EqualityAnd, 64);
BENCHMARK_TEMPLATE(implStdBitset_EqualityAnd, 64);
BENCHMARK_TEMPLATE(implMongoBitset_EqualityAnd, 1, 64);
BENCHMARK_TEMPLATE(implMongoBitset_EqualityAnd, 2, 64);
BENCHMARK_TEMPLATE(implBoostBitset_EqualityAnd, 128);
BENCHMARK_TEMPLATE(implStdBitset_EqualityAnd, 128);
BENCHMARK_TEMPLATE(implBoostBitset_EqualityAnd, 256);
BENCHMARK_TEMPLATE(implStdBitset_EqualityAnd, 256);
BENCHMARK_TEMPLATE(implBoostBitset_EqualityAnd, 512);
BENCHMARK_TEMPLATE(implStdBitset_EqualityAnd, 512);

BENCHMARK_TEMPLATE(implBitset_removeRedundancies, MintermStdBitset)
    ->Args({4})
    ->Args({16})
    ->Args({500})
    ->Args({1000})
    ->Args({2000});
BENCHMARK_TEMPLATE(implBitset_removeRedundancies, MintermMongoBitset)
    ->Args({4})
    ->Args({16})
    ->Args({500})
    ->Args({1000})
    ->Args({2000});
BENCHMARK_TEMPLATE(implBitset_removeRedundancies, MintermBoostBitset)
    ->Args({4})
    ->Args({16})
    ->Args({500})
    ->Args({1000})
    ->Args({2000});

BENCHMARK_TEMPLATE(implStdBitset_CountNumberOfSetBits, 64);
BENCHMARK_TEMPLATE(implMongoBitset_CountNumberOfSetBits, 64);
BENCHMARK_TEMPLATE(implBoostBitset_CountNumberOfSetBits, 64);
BENCHMARK_TEMPLATE(implStdBitset_CountNumberOfSetBits, 128);
BENCHMARK_TEMPLATE(implMongoBitset_CountNumberOfSetBits, 128);
BENCHMARK_TEMPLATE(implBoostBitset_CountNumberOfSetBits, 128);
BENCHMARK_TEMPLATE(implStdBitset_CountNumberOfSetBits, 256);
BENCHMARK_TEMPLATE(implMongoBitset_CountNumberOfSetBits, 256);
BENCHMARK_TEMPLATE(implBoostBitset_CountNumberOfSetBits, 256);

BENCHMARK_TEMPLATE(implStdBitset_CountNumberOfEmptySetBits, 64);
BENCHMARK_TEMPLATE(implMongoBitset_CountNumberOfEmptySetBits, 64);
BENCHMARK_TEMPLATE(implBoostBitset_CountNumberOfEmptySetBits, 64);
BENCHMARK_TEMPLATE(implStdBitset_CountNumberOfEmptySetBits, 128);
BENCHMARK_TEMPLATE(implMongoBitset_CountNumberOfEmptySetBits, 128);
BENCHMARK_TEMPLATE(implBoostBitset_CountNumberOfEmptySetBits, 128);
BENCHMARK_TEMPLATE(implStdBitset_CountNumberOfEmptySetBits, 256);
BENCHMARK_TEMPLATE(implMongoBitset_CountNumberOfEmptySetBits, 256);
BENCHMARK_TEMPLATE(implBoostBitset_CountNumberOfEmptySetBits, 256);

BENCHMARK(implMongoBitset_Any);
BENCHMARK(implMongoBitset_All);
}  // namespace mongo::boolean_simplification
