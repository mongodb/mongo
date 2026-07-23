// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/ce/hyperloglog.h"

#include "mongo/unittest/unittest.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace mongo::ce {
namespace {

/**
 * SplitMix64 mixing function. It is a bijection on 64-bit integers whose outputs are
 * statistically indistinguishable from uniform, so it serves as the ideal hash function for
 * exercising the sketch: n distinct inputs yield exactly n distinct, uniformly distributed
 * hashes, giving a deterministic ground truth.
 */
uint64_t splitmix64(uint64_t x) {
    x += 0x9e3779b97f4a7c15;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9;
    x = (x ^ (x >> 27)) * 0x94d049bb133111eb;
    return x ^ (x >> 31);
}

/**
 * Builds a sketch over the hashes of 'count' distinct inputs starting at 'start'.
 */
HyperLogLog makeSketch(size_t precision, uint64_t start, uint64_t count) {
    HyperLogLog hll = uassertStatusOK(HyperLogLog::create(precision));
    for (uint64_t i = 0; i < count; ++i) {
        hll.addHash(splitmix64(start + i));
    }
    return hll;
}

double relativeError(double estimate, double actual) {
    return std::abs(estimate - actual) / actual;
}

bool sameRegisters(const HyperLogLog& a, const HyperLogLog& b) {
    return std::ranges::equal(a.registers(), b.registers());
}

TEST(HyperLogLogTest, PrecisionMustBeInSupportedRange) {
    ASSERT_EQ(HyperLogLog::create(HyperLogLog::kMinPrecision - 1).getStatus().code(),
              ErrorCodes::BadValue);
    ASSERT_EQ(HyperLogLog::create(HyperLogLog::kMaxPrecision + 1).getStatus().code(),
              ErrorCodes::BadValue);

    for (size_t precision : {HyperLogLog::kMinPrecision, HyperLogLog::kMaxPrecision}) {
        HyperLogLog hll = uassertStatusOK(HyperLogLog::create(precision));
        ASSERT_EQ(hll.precision(), precision);
        ASSERT_EQ(hll.registers().size(), size_t{1} << precision);
        ASSERT_GTE(hll.getApproximateSize(), size_t{1} << precision);
    }
}

TEST(HyperLogLogTest, EmptySketchEstimatesZero) {
    auto hll = makeSketch(14, 0, 0);
    ASSERT_EQ(hll.estimate(), 0.0);
}

TEST(HyperLogLogTest, SingleHashEstimatesOne) {
    auto hll = makeSketch(14, 0, 0);
    hll.addHash(splitmix64(42));
    ASSERT_APPROX_EQUAL(hll.estimate(), 1.0, 0.01);
}

TEST(HyperLogLogTest, DuplicateHashesDoNotChangeState) {
    auto expected = makeSketch(14, 0, 1000);

    auto hll = makeSketch(14, 0, 0);
    for (int repetition = 0; repetition < 5; ++repetition) {
        for (uint64_t i = 0; i < 1000; ++i) {
            hll.addHash(splitmix64(i));
        }
    }
    ASSERT_TRUE(sameRegisters(hll, expected));
    ASSERT_EQ(hll.estimate(), expected.estimate());
}

TEST(HyperLogLogTest, InsertionOrderIsIrrelevant) {
    auto forward = makeSketch(14, 0, 10'000);

    auto backward = makeSketch(14, 0, 0);
    for (uint64_t i = 10'000; i-- > 0;) {
        backward.addHash(splitmix64(i));
    }
    ASSERT_TRUE(sameRegisters(forward, backward));
}

TEST(HyperLogLogTest, RegisterSelectionAndRank) {
    // With precision 14, the top 14 bits of the hash select the register and the rank is the
    // position of the leftmost 1-bit of the remaining 50 bits (or 51 if they are all zero).
    auto hll = makeSketch(14, 0, 0);

    // Register 5, all 50 remaining bits zero: rank 51.
    hll.addHash(uint64_t{5} << 50);
    // Register 0, only the lowest bit set: 49 leading zeros in the remaining bits, rank 50.
    hll.addHash(uint64_t{1});
    // Register 7, highest remaining bit set: rank 1.
    hll.addHash((uint64_t{7} << 50) | (uint64_t{1} << 49));

    ASSERT_EQ(hll.registers()[5], 51);
    ASSERT_EQ(hll.registers()[0], 50);
    ASSERT_EQ(hll.registers()[7], 1);
    ASSERT_EQ(std::ranges::count(hll.registers(), 0), (1 << 14) - 3);
}

TEST(HyperLogLogTest, LinearCountingRangeIsNearExact) {
    // Cardinalities far below the register count (16384 at precision 14) resolve via linear
    // counting over the empty registers, which is near-exact.
    {
        auto hll = makeSketch(14, 0, 10);
        ASSERT_APPROX_EQUAL(hll.estimate(), 10.0, 1.5);
    }
    for (uint64_t n : {100, 1'000, 10'000}) {
        auto hll = makeSketch(14, 0, n);
        double err = relativeError(hll.estimate(), n);
        ASSERT_LT(err, 0.02) << "n=" << n << " estimate=" << hll.estimate();
    }
}

TEST(HyperLogLogTest, LargeCardinalitiesWithinExpectedError) {
    // At precision 14 the standard error is 1.04/sqrt(2^14) = 0.81%; assert we stay within ~3
    // standard deviations. The inputs are fixed, so this cannot flake.
    for (uint64_t n : {100'000, 1'000'000, 5'000'000}) {
        auto hll = makeSketch(14, 0, n);
        double err = relativeError(hll.estimate(), n);
        ASSERT_LT(err, 0.025) << "n=" << n << " estimate=" << hll.estimate();
    }
}

TEST(HyperLogLogTest, TransitionRegionHasModestBias) {
    // Around 2.5 * 2^precision (~41k at precision 14) the estimator switches from linear counting
    // to the raw estimate and exhibits its worst-case bias. Pin it to stay modest.
    for (uint64_t n : {30'000, 41'000, 55'000}) {
        auto hll = makeSketch(14, 0, n);
        double err = relativeError(hll.estimate(), n);
        ASSERT_LT(err, 0.05) << "n=" << n << " estimate=" << hll.estimate();
    }
}

TEST(HyperLogLogTest, ErrorStatisticsOverManyTrials) {
    // Replicates the style of experiment from Heule et al. (2013), section 5.2, on a small scale:
    // many independent runs at a fixed cardinality, checking the error distribution. At precision
    // 12 (4096 registers) the standard error is 1.04/sqrt(4096) = 1.625%.
    constexpr int kTrials = 30;
    constexpr uint64_t kCardinality = 50'000;

    double sumSquaredError = 0;
    double sumSignedError = 0;
    double maxError = 0;
    for (int trial = 0; trial < kTrials; ++trial) {
        auto hll = makeSketch(12, static_cast<uint64_t>(trial) << 32, kCardinality);
        double signedError = (hll.estimate() - static_cast<double>(kCardinality)) / kCardinality;
        sumSquaredError += signedError * signedError;
        sumSignedError += signedError;
        maxError = std::max(maxError, std::abs(signedError));
    }

    ASSERT_LT(std::sqrt(sumSquaredError / kTrials), 0.025);  // RMS ~1.5 standard deviations.
    ASSERT_LT(std::abs(sumSignedError / kTrials), 0.008);    // The estimator is unbiased.
    ASSERT_LT(maxError, 0.052);                              // ~3.2 standard deviations.
}

TEST(HyperLogLogTest, WorksAcrossPrecisionRange) {
    {
        // Precision 4: only 16 registers, standard error 26%. Sanity-check the arithmetic.
        auto hll = makeSketch(4, 0, 10'000);
        ASSERT_LT(relativeError(hll.estimate(), 10'000), 0.8);
    }
    {
        // Precision 18: 262144 registers, standard error 0.2%.
        auto hll = makeSketch(18, 0, 1'000'000);
        ASSERT_LT(relativeError(hll.estimate(), 1'000'000), 0.01);
    }
}

TEST(HyperLogLogTest, MergeMatchesDirectSketchOfUnion) {
    // Overlapping input ranges: [0, 60k) and [40k, 100k) together cover [0, 100k).
    auto whole = makeSketch(14, 0, 100'000);

    auto left = makeSketch(14, 0, 60'000);
    auto right = makeSketch(14, 40'000, 60'000);
    left.merge(right);
    ASSERT_TRUE(sameRegisters(left, whole));
    ASSERT_EQ(left.estimate(), whole.estimate());

    // Merging in the opposite direction yields the same result.
    auto leftAgain = makeSketch(14, 0, 60'000);
    auto rightAgain = makeSketch(14, 40'000, 60'000);
    rightAgain.merge(leftAgain);
    ASSERT_TRUE(sameRegisters(rightAgain, whole));
}

TEST(HyperLogLogTest, MergeWithEmptySketch) {
    auto reference = makeSketch(14, 0, 5'000);

    auto populated = makeSketch(14, 0, 5'000);
    populated.merge(makeSketch(14, 0, 0));
    ASSERT_TRUE(sameRegisters(populated, reference));

    auto empty = makeSketch(14, 0, 0);
    empty.merge(reference);
    ASSERT_TRUE(sameRegisters(empty, reference));
}

TEST(HyperLogLogTest, SerializedStateRoundTrips) {
    auto original = makeSketch(14, 0, 10'000);

    auto swRestored = HyperLogLog::create(original.precision(), original.registers());
    ASSERT_OK(swRestored.getStatus());
    auto& restored = swRestored.getValue();
    ASSERT_TRUE(sameRegisters(restored, original));
    ASSERT_EQ(restored.estimate(), original.estimate());

    // A restored sketch remains fully functional: adding the same values is still a no-op and
    // merging behaves as if it had observed the original inputs.
    for (uint64_t i = 0; i < 10'000; ++i) {
        restored.addHash(splitmix64(i));
    }
    ASSERT_TRUE(sameRegisters(restored, original));

    auto more = makeSketch(14, 10'000, 5'000);
    auto expectedUnion = makeSketch(14, 0, 15'000);
    restored.merge(more);
    ASSERT_TRUE(sameRegisters(restored, expectedUnion));
}

TEST(HyperLogLogTest, RejectsInvalidSerializedState) {
    // Wrong register count for the precision.
    std::vector<uint8_t> tooSmall(1 << 12, 0);
    ASSERT_EQ(HyperLogLog::create(14, tooSmall).getStatus().code(), ErrorCodes::BadValue);

    // A register value exceeding 64 - precision + 1 cannot have been produced by addHash().
    std::vector<uint8_t> outOfRange(1 << 14, 0);
    outOfRange[123] = 52;  // Max valid rank at precision 14 is 51.
    ASSERT_EQ(HyperLogLog::create(14, outOfRange).getStatus().code(), ErrorCodes::BadValue);
    outOfRange[123] = 51;
    auto swBoundary = HyperLogLog::create(14, outOfRange);
    ASSERT_OK(swBoundary.getStatus());
    ASSERT_EQ(swBoundary.getValue().registers()[123], 51);

    // The precision is validated as well.
    std::vector<uint8_t> registers(1 << 3, 0);
    ASSERT_EQ(HyperLogLog::create(3, registers).getStatus().code(), ErrorCodes::BadValue);
}

}  // namespace
}  // namespace mongo::ce
