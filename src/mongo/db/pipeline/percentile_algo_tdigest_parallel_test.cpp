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

#include <algorithm>
#include <boost/random/normal_distribution.hpp>
#include <limits>
#include <random>

#include "mongo/db/pipeline/percentile_algo.h"
#include "mongo/db/pipeline/percentile_algo_tdigest.h"

#include "mongo/db/exec/document_value/value.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/unittest.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
namespace {

using std::vector;

vector<double> generateData(size_t n) {
    auto seed = 1680031021;  // arbitrary
    LOGV2(7492710, "{seed}", "generateData", "seed"_attr = seed);
    std::mt19937 generator(seed);
    boost::random::normal_distribution<double> dist(0.0 /* mean */, 0.5 /* sigma */);

    vector<double> inputs;
    inputs.reserve(n);

    for (size_t i = 0; i < n; ++i) {
        inputs.push_back(dist(generator));
    }
    return inputs;
}

// Yes, a binary search could be faster, but it's not worth doing it here.
int computeRank(const vector<double>& sorted, double value) {
    for (size_t i = 0; i < sorted.size(); ++i) {
        if (value <= sorted[i]) {
            return i;
        }
    }
    ASSERT(false) << value << " is larger than the max input " << sorted.back();
    return -1;
}

TEST(TDigestTestParallel, CombineEmpty) {
    std::unique_ptr<PercentileAlgorithm> empty = createTDigestDistributedClassic();
    dynamic_cast<PartialPercentile<Value>*>(empty.get())
        ->combine(dynamic_cast<PartialPercentile<Value>*>(empty.get())->serialize());

    ASSERT(!empty->computePercentile(0.5).has_value())
        << "Combining two empty digests should produce an empty digest";
}

TEST(TDigestTestParallel, CombineWithEmpty) {
    std::unique_ptr<PercentileAlgorithm> empty = createTDigestDistributedClassic();
    std::unique_ptr<PercentileAlgorithm> digest = createTDigestDistributedClassic();
    vector<double> inputs = generateData(10'000);
    for (double val : inputs) {
        digest->incorporate(val);
    }
    const double median = *(digest->computePercentile(0.5));

    auto emptyAsPartial = dynamic_cast<PartialPercentile<Value>*>(empty.get());
    auto digestAsPartial = dynamic_cast<PartialPercentile<Value>*>(digest.get());
    digestAsPartial->combine(emptyAsPartial->serialize());

    ASSERT_EQ(median, *(digest->computePercentile(0.5)))
        << "Combining with empty digest should not change the median";
}

TEST(TDigestTestParallel, CombineIntoEmpty) {
    std::unique_ptr<PercentileAlgorithm> empty = createTDigestDistributedClassic();
    std::unique_ptr<PercentileAlgorithm> digest = createTDigestDistributedClassic();
    vector<double> inputs = generateData(10'000);
    for (double val : inputs) {
        digest->incorporate(val);
    }
    const double median = *(digest->computePercentile(0.5));

    auto emptyAsPartial = dynamic_cast<PartialPercentile<Value>*>(empty.get());
    auto digestAsPartial = dynamic_cast<PartialPercentile<Value>*>(digest.get());
    emptyAsPartial->combine(digestAsPartial->serialize());

    ASSERT_EQ(median, *(digest->computePercentile(0.5)))
        << "Combining into empty digest should not change the median";
}

TEST(TDigestTestParallel, CombineTwoNonEmpty) {
    std::unique_ptr<PercentileAlgorithm> d1 = createTDigestDistributedClassic();
    std::unique_ptr<PercentileAlgorithm> d2 = createTDigestDistributedClassic();
    vector<double> inputs = generateData(10'000);
    for (size_t i = 0; i < inputs.size() / 2; i++) {
        d1->incorporate(inputs[i]);
    }
    for (size_t i = inputs.size() / 2; i < inputs.size(); ++i) {
        d2->incorporate(inputs[i]);
    }

    auto partial1 = dynamic_cast<PartialPercentile<Value>*>(d1.get());
    auto partial2 = dynamic_cast<PartialPercentile<Value>*>(d2.get());
    partial1->combine(partial2->serialize());

    std::sort(inputs.begin(), inputs.end());

    // Min and max are computed precisely
    ASSERT_EQ(inputs.front(), *(d1->computePercentile(0.0))) << "Min after combining";
    ASSERT_EQ(inputs.back(), *(d1->computePercentile(1.0))) << "Max after combining";

    const double median = *(d1->computePercentile(0.5));
    const int rank = computeRank(inputs, median);
    ASSERT_APPROX_EQUAL(0.5 * inputs.size(), rank, 100) << "Median after combining: " << median;
}

TEST(TDigestTestParallel, CombineInfinities) {
    const double inf = std::numeric_limits<double>::infinity();

    std::unique_ptr<PercentileAlgorithm> d1 = createTDigestDistributedClassic();
    std::unique_ptr<PercentileAlgorithm> d2 = createTDigestDistributedClassic();
    vector<double> inputs = generateData(10'000);
    for (size_t i = 0; i < inputs.size() / 2; i++) {
        d1->incorporate(inputs[i]);
    }
    d1->incorporate(inf);
    for (size_t i = inputs.size() / 2; i < inputs.size(); ++i) {
        d2->incorporate(inputs[i]);
    }
    d2->incorporate(-inf);

    auto partial1 = dynamic_cast<PartialPercentile<Value>*>(d1.get());
    auto partial2 = dynamic_cast<PartialPercentile<Value>*>(d2.get());
    partial1->combine(partial2->serialize());

    std::sort(inputs.begin(), inputs.end());

    // Min and max are computed precisely
    ASSERT_EQ(-inf, *(d1->computePercentile(0.0))) << "Min after combining";
    ASSERT_EQ(inf, *(d1->computePercentile(1.0))) << "Max after combining";

    const double median = *(d1->computePercentile(0.5));
    const int rank = computeRank(inputs, median);
    ASSERT_APPROX_EQUAL(0.5 * inputs.size(), rank, 100) << "Median after combining: " << median;
}

}  // namespace
}  // namespace mongo
