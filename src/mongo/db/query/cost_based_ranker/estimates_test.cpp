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

#include "mongo/db/query/cost_based_ranker/estimates.h"

#include "mongo/unittest/assert.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/framework.h"

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
    ASSERT_EQUALS(cc1.toBSON().toString(), "{ Cost coefficient: 5e-05.0, Source: \"Code\" }");
    ASSERT_EQUALS(cc2.toBSON().toString(), "{ Cost coefficient: 0.015, Source: \"Code\" }");
}

TEST(EstimatesFramework, SourceMerge) {
    CostCoefficient cc1(CostCoefficientType{0.00042});
    CardinalityEstimate ce1(CardinalityType{314298.78}, EstimationSource::Histogram);
    auto cst1 = cc1 * ce1;
    // Combining kSource with any other source leaves that source
    ASSERT_EQUALS(cst1.source(), EstimationSource::Histogram);

    // Combining any other two source types results in a mixed source
    CostEstimate cst2(CostType{123.213}, EstimationSource::Heuristics);
    auto cst3 = cst2 + cst1;
    ASSERT_EQUALS(cst3.source(), EstimationSource::Mixed);

    CardinalityEstimate ce2(CardinalityType{914598.1}, EstimationSource::Histogram);
    auto ce3 = ce1 + ce2;
    ASSERT_EQUALS(ce3.source(), ce1.source());
    ASSERT_EQUALS(ce3.source(), ce2.source());
}

}  // unnamed namespace
}  // namespace mongo::cost_based_ranker
