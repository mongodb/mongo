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

#include <vector>

#include "mongo/db/query/ce/cbp_histogram_ce/array_histogram_helpers.h"
#include "mongo/db/query/ce/cbp_histogram_ce/histogram_common.h"
#include "mongo/db/query/ce/cbp_histogram_ce/histogram_predicate_estimation.h"
#include "mongo/db/query/ce/cbp_histogram_ce/scalar_histogram_helpers.h"
#include "mongo/db/query/ce/cbp_histogram_ce/test_utils.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/framework.h"

namespace mongo::optimizer::cbp::ce {
namespace {

namespace value = sbe::value;

using mongo::Interval;
using stats::ArrayHistogram;
using stats::ScalarHistogram;
using stats::TypeCounts;

auto NumberInt64 = sbe::value::TypeTags::NumberInt64;

TEST(HistogramPredicateEstimationTest, IntHistogramIntervalEstimation) {
    std::vector<BucketData> data{{0, 1.0, 1.0, 1.0},
                                 {10, 1.0, 10.0, 5.0},
                                 {20, 3.0, 15.0, 3.0},
                                 {30, 1.0, 10.0, 4.0},
                                 {40, 2.0, 0.0, 0.0},
                                 {50, 1.0, 10.0, 5.0}};
    const Cardinality intCnt = 55;
    const ScalarHistogram hist = createHistogram(data);
    const auto arrHist = ArrayHistogram::make(hist, TypeCounts{{NumberInt64, intCnt}}, intCnt);

    {  // {a: 20}
        Interval interval(BSON("" << 20 << "" << 20), true /*startIncluded*/, true /*endIncluded*/);
        auto estimatedCard =
            estimateCardinalityEq(*arrHist, NumberInt64, 20, true /*includeScalar*/).card;
        ASSERT_EQ(3.0, estimatedCard);
        ASSERT_CE_APPROX_EQUAL(estimatedCard,
                               estimateIntervalCardinality(*arrHist, interval),
                               0.001 /* rounding error */);
    }

    {  // {a: {$gte: 20, $lte: 30}}
        Interval interval(BSON("" << 20 << "" << 30), true, true);
        auto estimatedCard = estimateCardinalityRange(*arrHist,
                                                      true /*lowInclusive*/,
                                                      NumberInt64,
                                                      20,
                                                      true /*highInclusive*/,
                                                      NumberInt64,
                                                      30,
                                                      true /*includeScalar*/)
                                 .card;
        ASSERT_EQ(14.0, estimatedCard);
        ASSERT_CE_APPROX_EQUAL(estimatedCard,
                               estimateIntervalCardinality(*arrHist, interval),
                               0.001 /* rounding error */);
    }

    {  //  {a: {$gte: 20, $lte: 25}}, bucket interpolation.
        Interval interval(BSON("" << 20 << "" << 25), true /*startIncluded*/, true /*endIncluded*/);
        auto estimatedCard = estimateCardinalityRange(*arrHist,
                                                      true /*lowInclusive*/,
                                                      NumberInt64,
                                                      20,
                                                      true /*highInclusive*/,
                                                      NumberInt64,
                                                      25,
                                                      true /*includeScalar*/)
                                 .card;
        ASSERT_EQ(8.0, estimatedCard);
        ASSERT_CE_APPROX_EQUAL(estimatedCard,
                               estimateIntervalCardinality(*arrHist, interval),
                               0.001 /* rounding error */);
    }

    {  // {a: {$and: [{$gte: 30}, {$lte: 40}]}}
        Interval interval(BSON("" << 30 << "" << 40), true /*startIncluded*/, true /*endIncluded*/);
        ASSERT_EQ(3.0,
                  HistogramCardinalityEstimator::estimateCardinality(
                      *arrHist, intCnt, interval, true /*includeScalar*/));
    }
}

TEST(HistogramPredicateEstimationTest, StrHistogramIntervalEstimation) {
    std::vector<BucketData> data{{
        {"0ejz", 1, 0, 0},
        {"4FGjc", 3, 5, 3},
        {"9bU3", 2, 3, 2},
        {"Cy5Kw", 3, 3, 3},
        {"Lm4U", 2, 11, 5},
        {"TTV", 5, 14, 8},
        {"YtzS", 2, 3, 2},
        {"o9cD4", 6, 26, 16},
        {"qfmnP", 1, 4, 2},
        {"xqbi", 2, 4, 4},
    }};
    const Cardinality strCnt = 100;
    const ScalarHistogram& hist = createHistogram(data);

    const auto arrHist = ArrayHistogram::make(
        hist, stats::TypeCounts{{sbe::value::TypeTags::StringSmall, strCnt}}, strCnt);

    auto [tagLow, valLow] = value::makeNewString("TTV"_sd);
    value::ValueGuard vgLow(tagLow, valLow);

    {  // {a: "TTV"}
        Interval interval(BSON(""
                               << "TTV"
                               << ""
                               << "TTV"),
                          true,
                          true);
        auto estimatedCard =
            estimateCardinalityEq(*arrHist, tagLow, valLow, true /*includeScalar*/).card;
        ASSERT_EQ(5.0, estimatedCard);
        ASSERT_CE_APPROX_EQUAL(estimatedCard,
                               estimateIntervalCardinality(*arrHist, interval),
                               0.001 /* rounding error */);
    }

    {  // {a: {$gte: "TTV", $lte: "YtzS"}}
        auto [tagHigh, valHigh] = value::makeNewString("YtzS"_sd);
        value::ValueGuard vgHigh(tagHigh, valHigh);
        Interval interval(BSON(""
                               << "TTV"
                               << ""
                               << "YtzS"),
                          true,
                          true);
        auto estimatedCard = estimateCardinalityRange(*arrHist,
                                                      true /*lowInclusive*/,
                                                      tagLow,
                                                      valLow,
                                                      true /*highInclusive*/,
                                                      tagHigh,
                                                      valHigh,
                                                      true /*includeScalar*/)
                                 .card;
        ASSERT_EQ(10.0, estimatedCard);
        ASSERT_CE_APPROX_EQUAL(estimatedCard,
                               estimateIntervalCardinality(*arrHist, interval),
                               0.001 /* rounding error */);
    }

    {  // {a: {$gte: "TTV", $lte: "VtzSlajdkajda"}} (tests for memory leaks for a large string)
        auto [tagHigh, valHigh] = value::makeNewString("VtzSlajdkajda"_sd);
        value::ValueGuard vgHigh(tagHigh, valHigh);
        Interval interval(BSON(""
                               << "TTV"
                               << ""
                               << "VtzSlajdkajda"),
                          true,
                          true);
        auto estimatedCard = estimateCardinalityRange(*arrHist,
                                                      true /*lowInclusive*/,
                                                      tagLow,
                                                      valLow,
                                                      true /*highInclusive*/,
                                                      tagHigh,
                                                      valHigh,
                                                      true /*includeScalar*/)
                                 .card;
        ASSERT_CE_APPROX_EQUAL(6.244, estimatedCard, 0.001);
        ASSERT_CE_APPROX_EQUAL(estimatedCard,
                               estimateIntervalCardinality(*arrHist, interval),
                               0.001 /* rounding error */);
    }

    {  // {a: {$and: [{$gte: "YtzS"}, {$lte: "o9cD4"}]}]}}
        Interval interval(BSON(""
                               << "YtzS"
                               << ""
                               << "o9cD4"),
                          true,
                          true);
        ASSERT_EQ(34.0,
                  HistogramCardinalityEstimator::estimateCardinality(
                      *arrHist, strCnt, interval, true /*includeScalar*/));
    }
}

TEST(HistogramPredicateEstimationTest, IntStrHistogramIntervalEstimation) {
    std::vector<BucketData> data{{
        {2, 3, 0, 0},       {19, 4, 1, 1},      {226, 2, 49, 20},  {301, 5, 12, 4},
        {317, 3, 0, 0},     {344, 2, 3, 1},     {423, 5, 18, 6},   {445, 3, 0, 0},
        {495, 3, 4, 2},     {542, 5, 9, 3},     {696, 3, 44, 19},  {773, 4, 11, 5},
        {805, 2, 8, 4},     {931, 5, 21, 8},    {998, 4, 21, 3},   {"8N4", 5, 31, 14},
        {"MIb", 5, 45, 17}, {"Zgi", 3, 55, 22}, {"pZ", 6, 62, 25}, {"yUwxz", 5, 29, 12},
    }};
    const Cardinality intCnt = 254;
    const Cardinality strCnt = 246;
    const Cardinality totalCnt = intCnt + strCnt;
    const ScalarHistogram& hist = createHistogram(data);

    ASSERT_EQ(totalCnt, getTotals(hist).card);

    const auto arrHist = ArrayHistogram::make(
        hist,
        stats::TypeCounts{{NumberInt64, intCnt}, {sbe::value::TypeTags::StringSmall, strCnt}},
        totalCnt);

    {  // {a: 993}
        Interval interval(BSON("" << 993 << "" << 993), true, true);
        auto estimatedCard =
            estimateCardinalityEq(*arrHist, NumberInt64, 993, true /*includeScalar*/).card;
        ASSERT_APPROX_EQUAL(7.0, estimatedCard,
                            0.1);  // Actual: 9
        ASSERT_CE_APPROX_EQUAL(estimatedCard,
                               estimateIntervalCardinality(*arrHist, interval),
                               0.001 /* rounding error */);
    }

    {  // {a: "04e"}
        auto [tag, value] = sbe::value::makeNewString("04e"_sd);
        sbe::value::ValueGuard vg(tag, value);
        Interval interval(BSON(""
                               << "04e"
                               << ""
                               << "04e"),
                          true /*startIncluded*/,
                          true /*endIncluded*/);
        auto estimatedCard =
            estimateCardinalityEq(*arrHist, tag, value, true /*includeScalar*/).card;
        ASSERT_APPROX_EQUAL(2.2, estimatedCard, 0.1);  // Actual: 3.
        ASSERT_CE_APPROX_EQUAL(estimatedCard,
                               estimateIntervalCardinality(*arrHist, interval),
                               0.001 /* rounding error */);
    }

    {  // {a: 100000000}
        value::TypeTags tagLow = NumberInt64;
        value::Value valLow = 100000000;
        Interval interval(
            BSON("" << 100000000 << "" << 100000000), true /*startIncluded*/, true /*endIncluded*/);
        auto estimatedCard =
            estimateCardinalityEq(*arrHist, tagLow, valLow, true /*includeScalar*/).card;
        ASSERT_APPROX_EQUAL(0.0, estimatedCard,
                            0.1);  // Actual: 0
        ASSERT_CE_APPROX_EQUAL(estimatedCard,
                               estimateIntervalCardinality(*arrHist, interval),
                               0.001 /* rounding error */);
    }

    {  // {$match: {a: {$lt: '04e'}}}
        auto [tagLow, valLow] = sbe::value::makeNewString(""_sd);
        auto [tagHigh, valHigh] = sbe::value::makeNewString("04e"_sd);
        sbe::value::ValueGuard vgLow(tagLow, valLow);
        sbe::value::ValueGuard vgHigh(tagHigh, valHigh);
        Interval interval(BSON(""
                               << ""
                               << ""
                               << "04e"),
                          true,
                          false);
        auto estimatedCard = estimateCardinalityRange(*arrHist,
                                                      false /* lowInclusive */,
                                                      tagLow,
                                                      valLow,
                                                      false /* highInclusive */,
                                                      tagHigh,
                                                      valHigh,
                                                      true /* includeScalar */)
                                 .card;
        ASSERT_CE_APPROX_EQUAL(13.3, estimatedCard, 0.1);  // Actual: 0.
        ASSERT_CE_APPROX_EQUAL(estimatedCard,
                               estimateIntervalCardinality(*arrHist, interval),
                               0.001 /* rounding error */);
    }
}

TEST(HistogramPredicateEstimationTest, IntArrayOnlyIntervalEstimate) {
    // This hard-codes a maxdiff histogram with 10 buckets built off of an array distribution with
    // arrays between 3 and 5 elements long, each containing 100 distinct ints uniformly distributed
    // between 0 and 1000. There are no scalar elements.
    std::vector<BucketData> scalarData{{}};
    const ScalarHistogram scalarHist = createHistogram(scalarData);

    std::vector<BucketData> minData{{
        {5, 3, 0, 0},   {19, 5, 2, 1},  {57, 4, 4, 3},  {116, 7, 13, 7}, {198, 3, 15, 6},
        {228, 2, 3, 2}, {254, 4, 0, 0}, {280, 2, 2, 1}, {335, 3, 5, 3},  {344, 2, 0, 0},
        {388, 3, 0, 0}, {420, 2, 0, 0}, {454, 1, 6, 3}, {488, 2, 1, 1},  {530, 1, 0, 0},
        {561, 1, 0, 0}, {609, 1, 0, 0}, {685, 1, 0, 0}, {713, 1, 0, 0},  {758, 1, 0, 0},
    }};
    const ScalarHistogram minHist = createHistogram(minData);

    std::vector<BucketData> maxData{{
        {301, 1, 0, 0},  {408, 2, 0, 0}, {445, 1, 0, 0}, {605, 2, 0, 0},  {620, 1, 0, 0},
        {665, 1, 1, 1},  {687, 3, 0, 0}, {704, 2, 6, 2}, {718, 2, 2, 1},  {741, 2, 1, 1},
        {752, 2, 0, 0},  {823, 7, 3, 3}, {827, 1, 0, 0}, {852, 3, 0, 0},  {864, 5, 0, 0},
        {909, 7, 12, 5}, {931, 2, 3, 1}, {939, 3, 0, 0}, {970, 2, 12, 4}, {998, 1, 10, 4},
    }};
    const ScalarHistogram maxHist = createHistogram(maxData);

    std::vector<BucketData> uniqueData{{
        {5, 3, 0, 0},    {19, 6, 2, 1},    {57, 4, 4, 3},    {116, 7, 15, 8},  {228, 2, 38, 13},
        {254, 7, 0, 0},  {269, 10, 0, 0},  {280, 7, 3, 1},   {306, 4, 1, 1},   {317, 4, 0, 0},
        {344, 2, 19, 5}, {423, 2, 27, 8},  {507, 2, 22, 13}, {704, 8, 72, 34}, {718, 6, 3, 1},
        {758, 3, 13, 4}, {864, 7, 35, 14}, {883, 4, 0, 0},   {939, 5, 32, 10}, {998, 1, 24, 9},
    }};

    const Cardinality totalCnt = 100.0;

    const ScalarHistogram uniqueHist = createHistogram(uniqueData);

    const auto arrHist = ArrayHistogram::make(scalarHist,
                                              TypeCounts{{value::TypeTags::Array, 100}},
                                              uniqueHist,
                                              minHist,
                                              maxHist,
                                              // There are 100 non-empty int-only arrays.
                                              TypeCounts{{value::TypeTags::NumberInt64, 100}},
                                              totalCnt /* sampleSize */);

    {  // {$match: {a: {$elemMatch: {$gt: 500, $lt: 600}}}}
        value::TypeTags tagLow = NumberInt64;
        value::Value valLow = 500;
        value::TypeTags tagHigh = NumberInt64;
        value::Value valHigh = 600;
        Interval interval(BSON("" << 500 << "" << 600), false, false /*endIncluded*/);
        auto estimatedCard = estimateCardinalityRange(*arrHist,
                                                      false /*lowInclusive*/,
                                                      tagLow,
                                                      valLow,
                                                      false /*highInclusive*/,
                                                      tagHigh,
                                                      valHigh,
                                                      false /*includeScalar*/)
                                 .card;
        ASSERT_CE_APPROX_EQUAL(27.0, estimatedCard, 0.1);  // actual 21.
        ASSERT_CE_APPROX_EQUAL(
            estimatedCard,
            estimateIntervalCardinality(*arrHist, interval, false /*includeScalar*/),
            0.001 /* rounding error */);
    }

    {  // {$match: {a: {$elemMatch: {$gt: 10, $lt: 110}}}
        Interval interval(
            BSON("" << 10 << "" << 110), false /*startIncluded*/, false /*endIncluded*/);
        ASSERT_CE_APPROX_EQUAL(24.1,
                               HistogramCardinalityEstimator::estimateCardinality(
                                   *arrHist, totalCnt, interval, false /*includeScalar*/),
                               0.1 /* rounding error*/);
    }
}


DEATH_TEST(HistogramPredicateEstimationTest,
           NonHistogrammableTypesEstimation,
           "Hit a MONGO_UNREACHABLE_TASSERT") {
    const int64_t startInstant = 1496777923LL;
    const int64_t endInstant = 1496864323LL;
    const Timestamp& startTs{Seconds(startInstant), 0};
    const Timestamp& endTs{Seconds(endInstant), 0};
    std::vector<BucketData> data{{1, 10.0, 0.0, 0.0},
                                 {10, 20.0, 0.0, 0.0},
                                 {Value(startTs), 20.0, 0.0, 0.0},
                                 {Value(endTs), 5.0, 0.0, 0.0}};
    const Cardinality totalCnt = 100;
    const ScalarHistogram& hist = createHistogram(data);

    const auto arrHist = ArrayHistogram::make(hist,
                                              stats::TypeCounts{{value::TypeTags::NumberInt64, 30},
                                                                {value::TypeTags::Timestamp, 25},
                                                                {value::TypeTags::Boolean, 25},
                                                                {value::TypeTags::Nothing, 5},
                                                                {value::TypeTags::Object, 15}},
                                              totalCnt,
                                              5,
                                              20);

    {  // check estimation for sbe::value::TypeTags::Boolean
        Interval interval(
            BSON("" << true << "" << true), true /*startIncluded*/, true /*endIncluded*/);
        ASSERT_EQ(true,
                  HistogramCardinalityEstimator::canEstimateInterval(
                      *arrHist, interval, true /*includeScalar*/));
        ASSERT_CE_APPROX_EQUAL(5, /*estimatedCard */
                               estimateIntervalCardinality(*arrHist, interval),
                               0.001 /* rounding error */);
    }

    {  // check estimation for sbe::value::TypeTags::Nothing
        Interval interval(
            BSON("" << BSONNULL << "" << BSONNULL), true /*startIncluded*/, true /*endIncluded*/);
        ASSERT_EQ(true,
                  HistogramCardinalityEstimator::canEstimateInterval(
                      *arrHist, interval, true /*includeScalar*/));
        ASSERT_CE_APPROX_EQUAL(5, /*estimatedCard ,*/
                               estimateIntervalCardinality(*arrHist, interval),
                               0.001 /* rounding error */);
    }

    {  // check estimation for sbe::value::TypeTags::Timestamp
        Interval interval(
            BSON("" << startTs << "" << endTs), true /*startIncluded*/, true /*endIncluded*/);
        ASSERT_EQ(true,
                  HistogramCardinalityEstimator::canEstimateInterval(
                      *arrHist, interval, true /*includeScalar*/));
        ASSERT_CE_APPROX_EQUAL(25, /*estimatedCard ,*/
                               estimateIntervalCardinality(*arrHist, interval),
                               0.001 /* rounding error */);
    }

    {  // check estimation for sbe::value::TypeTags::Object (expected to fail)
        Interval interval(BSON("" << BSON(""
                                          << "")
                                  << ""
                                  << BSON(""
                                          << "")),
                          true /*startIncluded*/,
                          true /*endIncluded*/);
        ASSERT_EQ(false,
                  HistogramCardinalityEstimator::canEstimateInterval(
                      *arrHist, interval, true /*includeScalar*/));
        ASSERT_THROWS_CODE(estimateIntervalCardinality(*arrHist, interval), DBException, 9163900);
    }

    {  // check estimation for [Null, true]
        Interval interval(
            BSON("" << BSONNULL << "" << true), true /*startIncluded*/, true /*endIncluded*/);
        ASSERT_EQ(true,
                  HistogramCardinalityEstimator::canEstimateInterval(
                      *arrHist, interval, true /*includeScalar*/));
        ASSERT_CE_APPROX_EQUAL(75, /*estimatedCard ,*/
                               estimateIntervalCardinality(*arrHist, interval),
                               0.001 /* rounding error */);
    }

    {  // check estimation for [false, timestamp]
        Interval interval(
            BSON("" << false << "" << endTs), true /*startIncluded*/, true /*endIncluded*/);
        ASSERT_EQ(true,
                  HistogramCardinalityEstimator::canEstimateInterval(
                      *arrHist, interval, true /*includeScalar*/));
        ASSERT_CE_APPROX_EQUAL(50, /*estimatedCard ,*/
                               estimateIntervalCardinality(*arrHist, interval),
                               0.001 /* rounding error */);
    }
}

}  // namespace
}  // namespace mongo::optimizer::cbp::ce
