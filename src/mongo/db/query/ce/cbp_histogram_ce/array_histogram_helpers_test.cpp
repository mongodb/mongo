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

#include "mongo/db/query/ce/test_utils.h"
#include <memory>
#include <tuple>
#include <vector>

#include "mongo/db/query/ce/cbp_histogram_ce/array_histogram_helpers.h"
#include "mongo/db/query/ce/cbp_histogram_ce/histogram_common.h"
#include "mongo/db/query/ce/cbp_histogram_ce/histogram_predicate_estimation.h"
#include "mongo/db/query/ce/cbp_histogram_ce/test_helpers.h"
#include "mongo/db/query/stats/maxdiff_test_utils.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

namespace mongo::optimizer::cbp::ce {

namespace {
namespace value = sbe::value;

using stats::ArrayHistogram;
using stats::ScalarHistogram;

auto NumberInt64 = sbe::value::TypeTags::NumberInt64;

TEST(ArrayHistogramHelpersTest, ManualHistogram) {
    std::vector<BucketData> data{{0, 1.0, 1.0, 1.0},
                                 {10, 1.0, 10.0, 5.0},
                                 {20, 3.0, 15.0, 3.0},
                                 {30, 1.0, 10.0, 4.0},
                                 {40, 2.0, 0.0, 0.0},
                                 {50, 1.0, 10.0, 5.0}};
    const double intCnt = 55;
    const ScalarHistogram& hist = createHistogram(data);
    const auto arrHist =
        ArrayHistogram::make(hist, stats::TypeCounts{{NumberInt64, intCnt}}, intCnt);

    ASSERT_EQ(3.0, estimateCardinalityEq(*arrHist, NumberInt64, 20, true).card);
    ASSERT_EQ(1.0, estimateCardinalityEq(*arrHist, NumberInt64, 50, true).card);
    ASSERT_EQ(0,
              estimateCardinalityEq(*arrHist, NumberInt64, 40, false)
                  .card);  // should be 2.0 for includeScalar: true
    // value not in data
    ASSERT_EQ(0, estimateCardinalityEq(*arrHist, NumberInt64, 60, true).card);
}

TEST(ArrayHistogramHelpersTest, UniformIntHistogram) {
    std::vector<BucketData> data{{2, 1, 0, 0},
                                 {57, 3, 2, 1},
                                 {179, 5, 10, 6},
                                 {317, 5, 9, 6},
                                 {344, 3, 0, 0},
                                 {558, 4, 19, 12},
                                 {656, 2, 4, 3},
                                 {798, 3, 7, 4},
                                 {951, 5, 17, 7},
                                 {986, 1, 0, 0}};
    const ScalarHistogram& hist = createHistogram(data);
    const double intCnt = 100;
    const auto arrHist =
        ArrayHistogram::make(hist, stats::TypeCounts{{NumberInt64, intCnt}}, intCnt);

    ASSERT_EQ(4.0, estimateCardinalityEq(*arrHist, NumberInt64, 558, true).card);
    ASSERT_APPROX_EQUAL(1.6,
                        estimateCardinalityEq(*arrHist, NumberInt64, 530, true).card,
                        0.1);  // Actual: 1.
    ASSERT_APPROX_EQUAL(1.6,
                        estimateCardinalityEq(*arrHist, NumberInt64, 400, true).card,
                        0.1);  // Actual: 1.
}

TEST(ArrayHistogramHelpersTest, NormalIntArrayHistogram) {
    std::vector<BucketData> data{{2, 1, 0, 0},
                                 {317, 8, 20, 15},
                                 {344, 2, 0, 0},
                                 {388, 3, 0, 0},
                                 {423, 4, 2, 2},
                                 {579, 4, 12, 8},
                                 {632, 3, 2, 1},
                                 {696, 3, 5, 3},
                                 {790, 5, 4, 2},
                                 {993, 1, 21, 9}};
    const ScalarHistogram hist = createHistogram(data);
    const double intCnt = 100;
    const auto arrHist =
        ArrayHistogram::make(hist, stats::TypeCounts{{NumberInt64, intCnt}}, intCnt);

    ASSERT_EQ(3.0, estimateCardinalityEq(*arrHist, NumberInt64, 696, true).card);
    ASSERT_APPROX_EQUAL(1.3,
                        estimateCardinalityEq(*arrHist, NumberInt64, 150, true).card,
                        0.1);  // Actual: 1.
}

TEST(ArrayHistogramHelpersTest, SkewedIntHistogram) {
    std::vector<BucketData> data{{0, 1.0, 1.0, 1.0},
                                 {10, 150.0, 10.0, 5.0},
                                 {20, 100.0, 14.0, 3.0},
                                 {30, 1.0, 10.0, 4.0},
                                 {40, 2.0, 0.0, 0.0},
                                 {50, 1.0, 10.0, 5.0}};
    const double intCnt = 300;
    const ScalarHistogram& hist = createHistogram(data);
    const auto arrHist =
        ArrayHistogram::make(hist, stats::TypeCounts{{NumberInt64, intCnt}}, intCnt);

    ASSERT_EQ(150.0, estimateCardinalityEq(*arrHist, NumberInt64, 10, true).card);
    ASSERT_EQ(100.0, estimateCardinalityEq(*arrHist, NumberInt64, 20, true).card);
    ASSERT_EQ(1.0, estimateCardinalityEq(*arrHist, NumberInt64, 30, true).card);
    ASSERT_EQ(0, estimateCardinalityEq(*arrHist, NumberInt64, 40, false).card);
}

TEST(ArrayHistogramHelpersTest, StringHistogram) {
    std::vector<BucketData> data{
        {"testA", 5.0, 2.0, 1.0}, {"testB", 3.0, 2.0, 2.0}, {"testC", 2.0, 1.0, 1.0}};
    const double strCnt = 15;
    const ScalarHistogram& hist = createHistogram(data);

    ASSERT_EQ(strCnt, getTotals(hist).card);

    const auto arrHist = ArrayHistogram::make(
        hist, stats::TypeCounts{{sbe::value::TypeTags::StringSmall, strCnt}}, strCnt);

    auto [tag, value] = sbe::value::makeNewString("testA"_sd);
    sbe::value::ValueGuard vg(tag, value);
    ASSERT_EQ(5.0, estimateCardinalityEq(*arrHist, tag, value, true).card);

    std::tie(tag, value) = sbe::value::makeNewString("testB"_sd);
    ASSERT_EQ(3.0, estimateCardinalityEq(*arrHist, tag, value, true).card);

    std::tie(tag, value) = sbe::value::makeNewString("testC"_sd);
    ASSERT_EQ(0, estimateCardinalityEq(*arrHist, tag, value, false).card);
}

TEST(ArrayHistogramHelpersTest, UniformStrHistogram) {
    std::vector<BucketData> data{{{"0ejz", 2, 0, 0},
                                  {"8DCaq", 3, 4, 4},
                                  {"Cy5Kw", 3, 3, 3},
                                  {"WXX7w", 3, 31, 20},
                                  {"YtzS", 2, 0, 0},
                                  {"fuK", 5, 13, 7},
                                  {"gLkp", 3, 0, 0},
                                  {"ixmVx", 2, 6, 2},
                                  {"qou", 1, 9, 6},
                                  {"z2b", 1, 9, 6}}};
    const double strCnt = 100;
    const ScalarHistogram& hist = createHistogram(data);

    const auto arrHist = ArrayHistogram::make(
        hist, stats::TypeCounts{{sbe::value::TypeTags::StringSmall, strCnt}}, strCnt);

    const auto [tag, value] = value::makeNewString("TTV"_sd);
    value::ValueGuard vg(tag, value);

    ASSERT_APPROX_EQUAL(
        1.55, estimateCardinalityEq(*arrHist, tag, value, true).card, 0.1);  // Actual: 2.
}

TEST(ArrayHistogramHelpersTest, NormalStrHistogram) {
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
    const double strCnt = 100;
    const ScalarHistogram& hist = createHistogram(data);

    const auto arrHist = ArrayHistogram::make(
        hist, stats::TypeCounts{{sbe::value::TypeTags::StringSmall, strCnt}}, strCnt);

    auto [tag, value] = value::makeNewString("TTV"_sd);
    value::ValueGuard vg(tag, value);

    ASSERT_APPROX_EQUAL(
        5.0, estimateCardinalityEq(*arrHist, tag, value, true).card, 0.1);  // Actual: 5.

    std::tie(tag, value) = value::makeNewString("Pfa"_sd);
    ASSERT_APPROX_EQUAL(
        1.75, estimateCardinalityEq(*arrHist, tag, value, true).card, 0.1);  // Actual: 2.
}

TEST(ArrayHistogramHelpersTest, IntStrHistogram) {
    std::vector<BucketData> data{{1, 1.0, 0.0, 0.0}, {"test", 20.0, 0.0, 0.0}};
    const double intCnt = 1;
    const double strCnt = 20;
    const double totalCnt = intCnt + strCnt;
    const ScalarHistogram& hist = createHistogram(data);

    ASSERT_EQ(totalCnt, getTotals(hist).card);

    const auto arrHist = ArrayHistogram::make(
        hist,
        stats::TypeCounts{{NumberInt64, intCnt}, {sbe::value::TypeTags::StringSmall, strCnt}},
        totalCnt);
    auto [tag, value] = sbe::value::makeNewString("test"_sd);
    sbe::value::ValueGuard vg(tag, value);

    ASSERT_EQ(20.0, estimateCardinalityEq(*arrHist, tag, value, true).card);
    ASSERT_EQ(1.0, estimateCardinalityEq(*arrHist, NumberInt64, 1, true).card);
    ASSERT_EQ(0, estimateCardinalityEq(*arrHist, tag, value, false).card);
    ASSERT_EQ(0, estimateCardinalityEq(*arrHist, NumberInt64, 1, false).card);
}

TEST(ArrayHistogramHelpersTest, UniformIntStrHistogram) {
    std::vector<BucketData> data{{
        {2, 3, 0, 0},       {19, 4, 1, 1},      {226, 2, 49, 20},  {301, 5, 12, 4},
        {317, 3, 0, 0},     {344, 2, 3, 1},     {423, 5, 18, 6},   {445, 3, 0, 0},
        {495, 3, 4, 2},     {542, 5, 9, 3},     {696, 3, 44, 19},  {773, 4, 11, 5},
        {805, 2, 8, 4},     {931, 5, 21, 8},    {998, 4, 21, 3},   {"8N4", 5, 31, 14},
        {"MIb", 5, 45, 17}, {"Zgi", 3, 55, 22}, {"pZ", 6, 62, 25}, {"yUwxz", 5, 29, 12},
    }};
    const double intCnt = 254;
    const double strCnt = 246;
    const double totalCnt = intCnt + strCnt;
    const ScalarHistogram& hist = createHistogram(data);

    ASSERT_EQ(totalCnt, getTotals(hist).card);

    const auto arrHist = ArrayHistogram::make(
        hist,
        stats::TypeCounts{{NumberInt64, intCnt}, {sbe::value::TypeTags::StringSmall, strCnt}},
        totalCnt);

    ASSERT_APPROX_EQUAL(7.0,
                        estimateCardinalityEq(*arrHist, NumberInt64, 993, true).card,
                        0.1);  // Actual: 9

    auto [tag, value] = sbe::value::makeNewString("04e"_sd);
    sbe::value::ValueGuard vg(tag, value);

    ASSERT_APPROX_EQUAL(
        2.2, estimateCardinalityEq(*arrHist, tag, value, true).card, 0.1);  // Actual: 3.

    ASSERT_APPROX_EQUAL(0.0,
                        estimateCardinalityEq(*arrHist, NumberInt64, 100000000, true).card,
                        0.1);  // Actual: 0
}

}  // namespace
}  // namespace mongo::optimizer::cbp::ce
