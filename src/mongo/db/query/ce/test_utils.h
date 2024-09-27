/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/bson/json.h"
#include "mongo/db/exec/docval_to_sbeval.h"
#include "mongo/db/query/ce/histogram_estimator.h"
#include "mongo/db/query/stats/max_diff.h"
#include "mongo/db/query/stats/maxdiff_test_utils.h"
#include "mongo/db/query/stats/rand_utils_new.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/assert.h"

namespace mongo::ce {

// Enable this flag to log all estimates, and let all tests pass.
constexpr bool kCETestLogOnly = false;

const double kMaxCEError = 0.01;

#define ASSERT_CE_APPROX_EQUAL(estimatedCE, expectedCE, kMaxCEError) \
    ASSERT_APPROX_EQUAL(                                             \
        static_cast<double>(estimatedCE), static_cast<double>(expectedCE), kMaxCEError)

template <class T1, class T2>
constexpr double absCEDiff(const T1 v1, const T2 v2) {
    return std::abs(static_cast<double>(v1) - static_cast<double>(v2));
}

/**
 * Helpful macros for asserting that the CE of a $match predicate is approximately what we were
 * expecting.
 */

#define _ASSERT_CE(estimatedCE, expectedCE)                             \
    if constexpr (kCETestLogOnly) {                                     \
        if (absCEDiff(estimatedCE, expectedCE) > kMaxCEError) {         \
            std::cout << "ERROR: expected " << expectedCE << std::endl; \
        }                                                               \
        ASSERT_APPROX_EQUAL(1.0, 1.0, kMaxCEError);                     \
    } else {                                                            \
        ASSERT_CE_APPROX_EQUAL(estimatedCE, expectedCE, kMaxCEError);   \
    }
#define _PREDICATE(field, predicate) (str::stream() << "{" << field << ": " << predicate "}")
#define _ELEMMATCH_PREDICATE(field, predicate) \
    (str::stream() << "{" << field << ": {$elemMatch: " << predicate << "}}")

// This macro verifies the cardinality of a pipeline or an input ABT.
#define ASSERT_CE(ce, pipeline, expectedCE) _ASSERT_CE(ce.getCE(pipeline), (expectedCE))

// This macro does the same as above but also sets the collection cardinality.
#define ASSERT_CE_CARD(ce, pipeline, expectedCE, collCard) \
    ce.setCollCard({collCard});                            \
    ASSERT_CE(ce, pipeline, expectedCE)

// This macro verifies the cardinality of a pipeline with a single $match predicate.
#define ASSERT_MATCH_CE(ce, predicate, expectedCE) \
    _ASSERT_CE(ce.getMatchCE(predicate), (expectedCE))

#define ASSERT_MATCH_CE_NODE(ce, queryPredicate, expectedCE, nodePredicate) \
    _ASSERT_CE(ce.getMatchCE(queryPredicate, nodePredicate), (expectedCE))

// This macro does the same as above but also sets the collection cardinality.
#define ASSERT_MATCH_CE_CARD(ce, predicate, expectedCE, collCard) \
    ce.setCollCard({collCard});                                   \
    ASSERT_MATCH_CE(ce, predicate, expectedCE)

// This macro tests cardinality of two versions of the predicate; with and without $elemMatch.
#define ASSERT_EQ_ELEMMATCH_CE(tester, expectedCE, elemMatchExpectedCE, field, predicate) \
    ASSERT_MATCH_CE(tester, _PREDICATE(field, predicate), expectedCE);                    \
    ASSERT_MATCH_CE(tester, _ELEMMATCH_PREDICATE(field, predicate), elemMatchExpectedCE)

#define ASSERT_EQ_ELEMMATCH_CE_NODE(tester, expectedCE, elemMatchExpectedCE, field, predicate, n) \
    ASSERT_MATCH_CE_NODE(tester, _PREDICATE(field, predicate), expectedCE, n);                    \
    ASSERT_MATCH_CE_NODE(tester, _ELEMMATCH_PREDICATE(field, predicate), elemMatchExpectedCE, n)

/**
 * Test utility for helping with creation of manual histograms in the unit tests.
 */
struct BucketData {
    Value _v;
    double _equalFreq;
    double _rangeFreq;
    double _ndv;

    BucketData(Value v, double equalFreq, double rangeFreq, double ndv)
        : _v(v), _equalFreq(equalFreq), _rangeFreq(rangeFreq), _ndv(ndv) {}
    BucketData(const std::string& v, double equalFreq, double rangeFreq, double ndv)
        : BucketData(Value(v), equalFreq, rangeFreq, ndv) {}
    BucketData(int v, double equalFreq, double rangeFreq, double ndv)
        : BucketData(Value(v), equalFreq, rangeFreq, ndv) {}
};

stats::ScalarHistogram createHistogram(const std::vector<BucketData>& data);
double estimateCardinalityScalarHistogramInteger(const stats::ScalarHistogram& hist,
                                                 int v,
                                                 EstimationType type);

}  // namespace mongo::ce
