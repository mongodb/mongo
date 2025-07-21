/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/query/compiler/optimizer/cost_based_ranker/estimates.h"

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
// DEATH_TEST(EstimatesFramework, NegativeCardinality, "Negative cardinality") {
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
    ASSERT_TRUE(ce1a <= ce2);
    ASSERT_TRUE(ce1a >= ce2);
    ASSERT_FALSE(ce1a > ce2);
    ASSERT_FALSE(ce1a < ce2);

    // Check the consistency of comparison operations with two estimates just different
    // enough not to compare equal.
    const CardinalityEstimate ce3(c3, defaultSrc);
    const CardinalityEstimate ce4(c4, defaultSrc);
    ASSERT_NOT_EQUALS(ce3, ce4);
    ASSERT_TRUE(ce3 < ce4);
    ASSERT_TRUE(ce3 <= ce4);
    ASSERT_TRUE(ce4 > ce3);
    ASSERT_TRUE(ce4 >= ce3);
    ASSERT_FALSE(ce4 < ce3);
    ASSERT_FALSE(ce4 <= ce3);

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
    ASSERT_TRUE(sel3 < sel1);
    ASSERT_TRUE(sel3 < sel2);

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
    ASSERT_EQUALS(cc1.toBSON().toString(),
                  "{ Cost coefficient: 9.999999999999999e-06, Source: \"Code\" }");
    ASSERT_EQUALS(cc2.toBSON().toString(), "{ Cost coefficient: 0.015, Source: \"Code\" }");
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

}  // unnamed namespace
}  // namespace mongo::cost_based_ranker
