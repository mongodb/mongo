// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/optimizer/cost_based_ranker/estimates.h"

#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo::cost_based_ranker {
namespace {

const auto defaultSrc = EstimationSource::Code;
// Minimum non-zero CE
const auto kMinNZCE = CardinalityType(std::numeric_limits<double>::min());

CardinalityEstimate makeCE(double ce) {
    return CardinalityEstimate(CardinalityType{ce}, defaultSrc);
}

// Test invalid inputs, and calculations that produce invalid estimates.
// Since the principle of checking validity is the same for all estimates, here we check only
// few typical cases.
// DEATH_TEST(EstimatesFrameworkDeathTest, NegativeCardinality, "Negative cardinality") {
//    const CardinalityEstimate ce1(CardinalityType{-7.0}, defaultSrc);
//}

TEST(EstimatesFramework, BinaryOp) {
    // TODO: An example where '==' double comparison fails for an expression that is
    // mathematically equal.
    auto c1 = CardinalityType{1.0e4 - 0.9};
    auto c2 = CardinalityType{1.0e4 + 0.9};
    auto c3 = CardinalityType{1.0e4};
    auto c4 = CardinalityType{1.0e4 + 2.1};
    auto c42 = CardinalityType{4242.4242};

    // Check the consistency of comparison operations with nearly equal values.
    const CardinalityEstimate ce1a(c1, defaultSrc);
    const CardinalityEstimate ce1b = ce1a;
    const CardinalityEstimate ce2(c2, defaultSrc);
    ASSERT_EQUALS(ce1a, ce2);
    ASSERT_EQUALS(ce1b, ce2);
    ASSERT_TRUE(approxLtEq(ce1a, ce2));
    ASSERT_TRUE(approxGtEq(ce1a, ce2));
    ASSERT_FALSE(approxGt(ce1a, ce2));
    ASSERT_FALSE(approxLt(ce1a, ce2));

    // Check the consistency of comparison operations with two estimates just different
    // enough not to compare equal.
    const CardinalityEstimate ce3(c3, defaultSrc);
    const CardinalityEstimate ce4(c4, defaultSrc);
    ASSERT_NOT_EQUALS(ce3, ce4);
    ASSERT_TRUE(approxLt(ce3, ce4));
    ASSERT_TRUE(approxLtEq(ce3, ce4));
    ASSERT_TRUE(approxGt(ce4, ce3));
    ASSERT_TRUE(approxGtEq(ce4, ce3));
    ASSERT_FALSE(approxLt(ce4, ce3));
    ASSERT_FALSE(approxLtEq(ce4, ce3));

    // Arithmetic operations
    const auto ce5 = zeroCE + ce3;
    const auto ce6 = ce3 - zeroCE;
    ASSERT_EQUALS(ce5, ce3);
    ASSERT_EQUALS(ce6, ce3);

    const CardinalityEstimate ce42(c42, defaultSrc);
    auto sum1 = ce1a + ce42 + ce3;
    auto sum2 = ce1a + ce3 + ce42;
    auto sum3 = ce42 + ce1a + ce3;
    auto sum4 = ce42 + ce3 + ce1a;
    auto sum5 = ce3 + ce42 + ce1a;
    auto sum6 = ce3 + ce1a + ce42;
    ASSERT_EQUALS(sum1, sum2);
    ASSERT_EQUALS(sum2, sum3);
    ASSERT_EQUALS(sum3, sum4);
    ASSERT_EQUALS(sum4, sum5);
    ASSERT_EQUALS(sum5, sum6);
    ASSERT_EQUALS(sum6, sum1);

    auto xce5 = sum1 - ce42;
    ASSERT_EQUALS(xce5, ce1a + ce3);
    xce5 = (ce1a + ce42 + ce3) - ce42 - ce3;
    ASSERT_EQUALS(xce5, ce1a);

    auto xce7 = 5.0 * ce2;
    xce5 = ce2 * 5.0;
    auto xce6 = ce2 + ce2 + ce2 - ce2 + ce2 + ce2 + ce2;
    ASSERT_EQUALS(xce7, xce6);
    ASSERT_EQUALS(xce5, xce6);

    xce7 = ce2;
    xce7 += ce2;
    ASSERT_EQUALS(xce7, ce2 * 2.0);
    xce7 -= ce2;
    ASSERT_EQUALS(xce7, ce2 * 1.0);

    // Combine cost and cardinality
    CostCoefficient cc1(CostCoefficientType{7 * CostCoefficientTag::kMinValue});
    CostCoefficient cc2(CostCoefficientType{CostCoefficientTag::kMaxValue / 13});
    auto cst1 = ce1a * cc1;
    auto cst2 = cc1 * ce1b;
    ASSERT_EQUALS(cst1, cst2);

    // Combine cardinality to produce selectivity
    auto sel1 = ce2 / (5.0 * ce4);
    ASSERT_TRUE(sel1 == SelectivityEstimate(SelectivityType{1.0 / 5.0}, defaultSrc));

    const auto ce8a = sel1 * xce7;
    const auto ce8b = xce7 * sel1;
    ASSERT_EQUALS(ce8a, ce8b);

    // Combine two selectivities
    auto sel2 = SelectivityEstimate(SelectivityType{0.00314}, EstimationSource::Sampling);
    auto sel3 = sel1 * sel2;
    ASSERT_TRUE(approxLt(sel3, sel1));
    ASSERT_TRUE(approxLt(sel3, sel2));

    SelectivityEstimate selMax(SelectivityType{1.0}, defaultSrc);
    auto sel4 = selMax * sel3;
    ASSERT_EQUALS(sel4, sel3);

    SelectivityEstimate selMin(SelectivityType{0.0}, defaultSrc);
    auto sel5 = selMin * sel2;
    ASSERT_EQUALS(sel5,
                  SelectivityEstimate(SelectivityType::minValue(), EstimationSource::Heuristics));
}

TEST(EstimatesFramework, PrintEstimates) {
    CardinalityEstimate ce1(CardinalityType{0.0}, EstimationSource::Histogram);
    ASSERT_EQUALS(ce1.toString(), "Cardinality: 0, Source: Histogram");
    ASSERT_EQUALS(ce1.toBSON().toString(), "{ Cardinality: 0.0, Source: \"Histogram\" }");

    CardinalityEstimate ce2(CardinalityType{100.0 / 3.33}, EstimationSource::Sampling);
    ASSERT_EQUALS(ce2.toBSON().toString(),
                  "{ Cardinality: 30.03003003003003, Source: \"Sampling\" }");

    CostCoefficient cc1(CostCoefficientType::minValue());
    CostCoefficient cc2(CostCoefficientType::maxValue());
    ASSERT_EQUALS(cc1.toBSON().toString(), "{ Cost coefficient: 1.167e-05, Source: \"Code\" }");
    ASSERT_EQUALS(cc2.toBSON().toString(), "{ Cost coefficient: 1.0, Source: \"Code\" }");
}

TEST(EstimatesFramework, SourceMerge) {
    CostCoefficient cc(CostCoefficientType{0.0042});  // Always of type Code
    CardinalityEstimate ce_hi(CardinalityType{1142.8}, EstimationSource::Histogram);
    CardinalityEstimate ce_sa(CardinalityType{2142.7}, EstimationSource::Sampling);
    CardinalityEstimate ce_he(CardinalityType{3142.3}, EstimationSource::Heuristics);
    CardinalityEstimate ce_mi(CardinalityType{4142.2}, EstimationSource::Mixed);
    CardinalityEstimate ce_me(CardinalityType{5142.1}, EstimationSource::Metadata);
    CardinalityEstimate ce_co(CardinalityType{5142.1}, EstimationSource::Code);

    // Test all combinations systematically

    // Code x Any = Any
    ASSERT_EQUALS((cc * ce_hi).source(), EstimationSource::Histogram);
    ASSERT_EQUALS((cc * ce_sa).source(), EstimationSource::Sampling);
    ASSERT_EQUALS((cc * ce_he).source(), EstimationSource::Heuristics);
    ASSERT_EQUALS((cc * ce_mi).source(), EstimationSource::Mixed);
    ASSERT_EQUALS((cc * ce_me).source(), EstimationSource::Metadata);
    ASSERT_EQUALS((cc * ce_co).source(), EstimationSource::Code);

    // Histogram x Any
    ASSERT_EQUALS((ce_hi + ce_hi).source(), EstimationSource::Histogram);
    ASSERT_EQUALS((ce_hi + ce_sa).source(), EstimationSource::Mixed);
    ASSERT_EQUALS((ce_hi + ce_he).source(), EstimationSource::Mixed);
    ASSERT_EQUALS((ce_hi + ce_mi).source(), EstimationSource::Mixed);
    ASSERT_EQUALS((ce_hi + ce_me).source(), EstimationSource::Histogram);
    ASSERT_EQUALS((ce_hi + ce_co).source(), EstimationSource::Histogram);

    // Sampling x Any
    ASSERT_EQUALS((ce_sa + ce_hi).source(), EstimationSource::Mixed);
    ASSERT_EQUALS((ce_sa + ce_sa).source(), EstimationSource::Sampling);
    ASSERT_EQUALS((ce_sa + ce_he).source(), EstimationSource::Mixed);
    ASSERT_EQUALS((ce_sa + ce_mi).source(), EstimationSource::Mixed);
    ASSERT_EQUALS((ce_sa + ce_me).source(), EstimationSource::Sampling);
    ASSERT_EQUALS((ce_sa + ce_co).source(), EstimationSource::Sampling);

    // Heuristics
    ASSERT_EQUALS((ce_he + ce_hi).source(), EstimationSource::Mixed);
    ASSERT_EQUALS((ce_he + ce_sa).source(), EstimationSource::Mixed);
    ASSERT_EQUALS((ce_he + ce_he).source(), EstimationSource::Heuristics);
    ASSERT_EQUALS((ce_he + ce_mi).source(), EstimationSource::Mixed);
    ASSERT_EQUALS((ce_he + ce_me).source(), EstimationSource::Heuristics);
    ASSERT_EQUALS((ce_he + ce_co).source(), EstimationSource::Heuristics);

    // Mixed
    ASSERT_EQUALS((ce_mi + ce_hi).source(), EstimationSource::Mixed);
    ASSERT_EQUALS((ce_mi + ce_sa).source(), EstimationSource::Mixed);
    ASSERT_EQUALS((ce_mi + ce_he).source(), EstimationSource::Mixed);
    ASSERT_EQUALS((ce_mi + ce_mi).source(), EstimationSource::Mixed);
    ASSERT_EQUALS((ce_mi + ce_me).source(), EstimationSource::Mixed);
    ASSERT_EQUALS((ce_mi + ce_co).source(), EstimationSource::Mixed);

    // Metadata
    ASSERT_EQUALS((ce_me + ce_hi).source(), EstimationSource::Histogram);
    ASSERT_EQUALS((ce_me + ce_sa).source(), EstimationSource::Sampling);
    ASSERT_EQUALS((ce_me + ce_he).source(), EstimationSource::Heuristics);
    ASSERT_EQUALS((ce_me + ce_mi).source(), EstimationSource::Mixed);
    ASSERT_EQUALS((ce_me + ce_me).source(), EstimationSource::Metadata);
    ASSERT_EQUALS((ce_me + ce_co).source(), EstimationSource::Metadata);

    // Test source merging across few different types of estimates and operations

    // Combining different source types results in a mixed source
    SelectivityEstimate s_he(SelectivityType{0.123}, EstimationSource::Heuristics);
    ASSERT_EQUALS((s_he * ce_hi).source(), EstimationSource::Mixed);

    // Combining Heuristics with Code should leave it unchanged and be symmetric.
    SelectivityEstimate s_co{SelectivityType{0.1}, EstimationSource::Code};
    ASSERT_EQ((s_co * ce_he).source(), EstimationSource::Heuristics);
    ASSERT_EQ((ce_he * s_co).source(), EstimationSource::Heuristics);

    // Combining selectivities with Metadata should leave it unchanged and be symmetric.
    SelectivityEstimate s1_me{SelectivityType{0}, EstimationSource::Metadata};
    SelectivityEstimate s2_me{SelectivityType{0}, EstimationSource::Heuristics};
    ASSERT_EQ((s1_me + s2_me).source(), EstimationSource::Heuristics);
    ASSERT_EQ((s2_me + s1_me).source(), EstimationSource::Heuristics);
}

// Subtraction must reconcile the estimates library's approximate comparison with its exact
// arithmetic. When the subtrahend is approximately equal to (but, in raw double terms, slightly
// larger than) the minuend, the result is clamped to the lower bound instead of tripping a tassert.
// This protects every '-' / '-=' call site (e.g. $not, $skip, XOR) from the comparison/arithmetic
// asymmetry.
TEST(EstimatesFramework, SubtractionClampsOnEpsilonEqualCardinality) {
    const CardinalityEstimate smaller = makeCE(1.0e6);
    const CardinalityEstimate larger = makeCE(1.0e6 + 50.0);
    // Approximately equal under the library's comparison, but exactly ordered the "wrong" way for
    // a safe subtraction.
    ASSERT_EQUALS(smaller, larger);
    ASSERT_TRUE(smaller.toDouble() < larger.toDouble());
    ASSERT_EQUALS(smaller - larger, zeroCE);
}

TEST(EstimatesFramework, SubtractionClampsOnEpsilonEqualSelectivity) {
    const SelectivityEstimate smaller{SelectivityType{0.5}, defaultSrc};
    const SelectivityEstimate larger{SelectivityType{0.5 + 0.001}, defaultSrc};
    ASSERT_EQUALS(smaller, larger);
    ASSERT_TRUE(smaller.toDouble() < larger.toDouble());
    ASSERT_EQUALS(smaller - larger, zeroSel);
}

// A genuine underflow (operands not approximately equal) is a real logic error and must still trip
// the tripwire assertion rather than being silently masked.
DEATH_TEST(EstimatesFrameworkDeathTest, SubtractionUnderflowStillAsserts, "12552500") {
    const CardinalityEstimate small = makeCE(10.0);
    const CardinalityEstimate big = makeCE(1000.0);
    [[maybe_unused]] auto bad = small - big;
}

// Symmetric upper-bound clamping. When the exact sum lands just past the maximum
// (within epsilon) the result clamps to the upper bound instead of tripping a tassert.
TEST(EstimatesFramework, AdditionClampsOnEpsilonOverMaxSelectivity) {
    const SelectivityEstimate a{SelectivityType{1.0}, defaultSrc};
    const SelectivityEstimate b{SelectivityType{0.005}, defaultSrc};
    ASSERT_EQUALS(a + b, oneSel);
}

DEATH_TEST(EstimatesFrameworkDeathTest, AdditionOverflowStillAsserts, "12552501") {
    const SelectivityEstimate a{SelectivityType{0.6}, defaultSrc};
    const SelectivityEstimate b{SelectivityType{0.6}, defaultSrc};
    [[maybe_unused]] auto bad = a + b;
}

// The upper-bound epsilon clamp masks only finite overshoots (e.g. selectivity 1.0 + 0.005). For a
// tag whose maximum is DBL_MAX (Cardinality), a sum that exceeds the maximum can only become +inf —
// never a finite value epsilon-above the max — and +inf is not nearlyEqual to DBL_MAX by any
// epsilon, so a genuine overflow still trips the assertion rather than being clamped.
DEATH_TEST(EstimatesFrameworkDeathTest, AdditionOverflowToInfinityStillAsserts, "12552501") {
    const CardinalityEstimate big = makeCE(std::numeric_limits<double>::max());
    [[maybe_unused]] auto bad = big + big;
}

// exactMin()/exactMax() compare the underlying values EXACTLY, so the result is guaranteed to be
// the not-greater (min) / not-smaller (max) operand regardless of argument order. This is unlike
// std::min/std::max, which derive from the approximate operator<=> and return their first argument
// on a fuzzy tie.
TEST(EstimatesFramework, ExactMinMaxBreakTiesByRawValue) {
    const auto larger = makeCE(1.0e6 + 50.0);
    const auto smaller = makeCE(1.0e6);
    // Approximately equal, but exactly ordered.
    ASSERT_EQUALS(larger, smaller);
    ASSERT_TRUE(smaller.toDouble() < larger.toDouble());

    // Compare the raw doubles exactly: exactMin must always return the smaller value, exactMax the
    // larger, independent of argument order.
    ASSERT_EQ(exactMin(larger, smaller).toDouble(), smaller.toDouble());
    ASSERT_EQ(exactMin(smaller, larger).toDouble(), smaller.toDouble());
    ASSERT_EQ(exactMax(larger, smaller).toDouble(), larger.toDouble());
    ASSERT_EQ(exactMax(smaller, larger).toDouble(), larger.toDouble());
}

// approxMin/approxMax are epsilon-tolerant and PAIRWISE. On a tie they return the second argument
// (mirroring the '(a < b) ? a : b' ternaries they replace), and unlike exactMin/exactMax they do
// not guarantee the result bounds both operands.
TEST(EstimatesFramework, ApproxMinMaxAreEpsilonTolerant) {
    const auto a = makeCE(1.0e6);
    const auto b = makeCE(1.0e6 + 50.0);  // within epsilon of 'a'
    ASSERT_EQUALS(a, b);
    // Tie -> returns the second argument, for both min and max.
    ASSERT_EQ(approxMin(a, b).toDouble(), b.toDouble());
    ASSERT_EQ(approxMax(a, b).toDouble(), b.toDouble());
    // Genuinely ordered operands behave like a normal min/max.
    const auto small = makeCE(10.0);
    const auto big = makeCE(1000.0);
    ASSERT_EQ(approxMin(big, small).toDouble(), small.toDouble());
    ASSERT_EQ(approxMax(small, big).toDouble(), big.toDouble());
}

// exactCompare distinguishes values that approxCompare treats as equivalent.
TEST(EstimatesFramework, ExactVsApproxCompare) {
    const auto a = makeCE(1.0e6);
    const auto b = makeCE(1.0e6 + 50.0);
    // approx: equivalent; exact: strictly ordered.
    ASSERT_TRUE(std::is_eq(approxCompare(a, b)));
    ASSERT_TRUE(std::is_lt(exactCompare(a, b)));
    ASSERT_TRUE(exactLt(a, b));
    ASSERT_FALSE(approxLt(a, b));
    ASSERT_TRUE(approxLtEq(a, b));
    ASSERT_TRUE(exactGt(b, a));
    ASSERT_TRUE(approxGtEq(a, b));
}

// Non-transitivity of approximate equivalence: a ~ b and b ~ c, yet a is NOT ~ c. A strict weak
// ordering requires transitive equivalence, so approxCompare is unsafe for ordered algorithms;
// exactCompare (a real ordering) is what std::sort must use.
TEST(EstimatesFramework, ApproxEquivalenceIsNotTransitive) {
    // Cardinality epsilon is 1e-4 (relative). With a ~1e6 baseline, an adjacent gap of 150 is
    // within epsilon (~7.5e-5) but the endpoint gap of 300 is not (~1.5e-4).
    const auto a = makeCE(1.0e6);
    const auto b = makeCE(1.0e6 + 150.0);
    const auto c = makeCE(1.0e6 + 300.0);
    ASSERT_TRUE(std::is_eq(approxCompare(a, b)));
    ASSERT_TRUE(std::is_eq(approxCompare(b, c)));
    ASSERT_FALSE(std::is_eq(approxCompare(a, c)));  // transitivity broken
    // The exact ordering has no such defect.
    ASSERT_TRUE(std::is_lt(exactCompare(a, c)));
}

// product() combines two cardinalities into a count x count product (e.g. nested-loop rows).
TEST(EstimatesFramework, ProductOfCardinalities) {
    ASSERT_EQUALS(product(makeCE(20.0), makeCE(50.0)), makeCE(1000.0));
}

// A cost scaled by a repetition count yields a cost; the operation is commutative.
TEST(EstimatesFramework, CostScaledByCardinality) {
    const CostEstimate c{CostType{2.5}, defaultSrc};
    const auto reps = makeCE(4.0);
    const CostEstimate expected{CostType{10.0}, defaultSrc};
    ASSERT_EQUALS(c * reps, expected);
    ASSERT_EQUALS(reps * c, expected);
}

// ratio() of two costs is a unitless double, not an estimate.
TEST(EstimatesFramework, CostRatioIsDimensionlessDouble) {
    const CostEstimate a{CostType{30.0}, defaultSrc};
    const CostEstimate b{CostType{10.0}, defaultSrc};
    ASSERT_EQ(ratio(a, b), 3.0);
}

// toCount() converts a cardinality to an integer by flooring (truncating toward zero).
TEST(EstimatesFramework, ToCountFloors) {
    ASSERT_EQ(makeCE(7.8).toCount(), 7);
    ASSERT_EQ(makeCE(7.0).toCount(), 7);
}

// saturatingSubtract() is a saturating ("truncated") subtraction for domains where the subtrahend
// legitimately exceeding the minuend means zero (e.g. $skip skipping past the end of the input). It
// never asserts, unlike operator-.
TEST(EstimatesFramework, SaturatingSubtractToZero) {
    // Subtrahend much larger than minuend (a genuine, non-epsilon difference): clamps to zero.
    ASSERT_EQUALS(saturatingSubtract(makeCE(10.0), makeCE(1000.0)), zeroCE);
    // Normal case subtracts exactly.
    ASSERT_EQUALS(saturatingSubtract(makeCE(1000.0), makeCE(10.0)), makeCE(990.0));
}

// operator/(CE, CE) must reconcile its approximate-equality callers with its exact precondition:
// when the numerator is exactly larger than the denominator but within epsilon, it yields a
// selectivity of 1.0 instead of tripping the exact precondition tassert.
TEST(EstimatesFramework, DivideEpsilonEqualCardinalitiesReturnsOne) {
    const auto numer = makeCE(1000.05);
    const auto denom = makeCE(1000.0);
    ASSERT_EQUALS(numer, denom);
    ASSERT_TRUE(numer.toDouble() > denom.toDouble());
    ASSERT_EQUALS(numer / denom, oneSel);
}

// A genuinely-larger numerator (not within epsilon) is a real logic error and still trips the
// exact precondition.
DEATH_TEST(EstimatesFrameworkDeathTest, DivideGenuinelyLargerStillAsserts, "9274202") {
    [[maybe_unused]] auto bad = makeCE(2000.0) / makeCE(1000.0);
}

}  // unnamed namespace
}  // namespace mongo::cost_based_ranker
