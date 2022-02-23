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

#include "mongo/util/histogram.h"

#include <boost/optional.hpp>

#include "mongo/unittest/assert_that.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {

using namespace unittest::match;
using namespace std::literals;

class HistogramTest : public unittest::Test {
public:
    template <typename T>
    struct BucketSpec {
        int64_t count;
        boost::optional<T> lower, upper;

        friend bool operator==(const BucketSpec& a, const BucketSpec& b) {
            auto lens = [](auto&& x) { return std::tie(x.count, x.lower, x.upper); };
            return lens(a) == lens(b);
        }

        friend std::ostream& operator<<(std::ostream& os, const BucketSpec& b) {
            os << "count: " << b.count;
            if (b.lower)
                os << ", lower: " << *b.lower;
            if (b.upper)
                os << ", upper: " << *b.upper;
            return os;
        }
    };

    template <typename T>
    boost::optional<T> ptrToOpt(const T* p) {
        return p ? boost::optional<T>(*p) : boost::none;
    }

    template <typename T>
    auto snapshot(const Histogram<T>& h) {
        std::vector<BucketSpec<T>> r;
        std::transform(h.begin(), h.end(), std::back_inserter(r), [&](auto&& b) {
            return BucketSpec<T>{b.count, ptrToOpt(b.lower), ptrToOpt(b.upper)};
        });
        return r;
    }
};

TEST_F(HistogramTest, CountsIncrementedAndStored) {
    Histogram<int64_t> hist({0, 5, 8, 12});
    for (int64_t i = 0; i < 15; ++i)
        hist.increment(i);
    std::vector<BucketSpec<int64_t>> expected = {
        {0, {}, 0}, {5, 0, 5}, {3, 5, 8}, {4, 8, 12}, {3, 12, {}}};

    ASSERT_THAT(snapshot(hist), Eq(expected));
}

TEST_F(HistogramTest, CountsIncrementedInSmallestBucket) {
    Histogram<int64_t> hist({5, 8, 12});
    for (int64_t i = 0; i < 5; ++i)
        hist.increment(i);
    std::vector<BucketSpec<int64_t>> expected = {{5, {}, 5}, {0, 5, 8}, {0, 8, 12}, {0, 12, {}}};

    ASSERT_THAT(snapshot(hist), Eq(expected));
}

TEST_F(HistogramTest, CountsIncrementedAtPartition) {
    std::vector<int64_t> origPartitions = {5, 8, 12};
    Histogram<int64_t> hist(origPartitions);
    for (auto& p : origPartitions)
        hist.increment(p);
    std::vector<BucketSpec<int64_t>> expected = {{0, {}, 5}, {1, 5, 8}, {1, 8, 12}, {1, 12, {}}};

    ASSERT_THAT(snapshot(hist), Eq(expected));
}

TEST_F(HistogramTest, NegativeValuesIncrementBuckets) {
    Histogram<int64_t> hist({-12, -8, 5});
    for (int64_t i = -15; i < 10; ++i)
        hist.increment(i);
    std::vector<BucketSpec<int64_t>> expected = {
        {3, {}, -12}, {4, -12, -8}, {13, -8, 5}, {5, 5, {}}};

    ASSERT_THAT(snapshot(hist), Eq(expected));
}

TEST_F(HistogramTest, DurationCountsIncrementedAndStored) {
    Histogram<Milliseconds> hist(
        {Milliseconds{0}, Milliseconds{5}, Milliseconds{8}, Milliseconds{12}});
    for (int64_t i = 0; i < 15; ++i)
        hist.increment(Milliseconds{i});
    std::vector<BucketSpec<Milliseconds>> expected = {{0, {}, Milliseconds{0}},
                                                      {5, Milliseconds{0}, Milliseconds{5}},
                                                      {3, Milliseconds{5}, Milliseconds{8}},
                                                      {4, Milliseconds{8}, Milliseconds{12}},
                                                      {3, Milliseconds{12}, {}}};

    ASSERT_THAT(snapshot(hist), Eq(expected));
}

TEST_F(HistogramTest, StringCountsIncrementedAndStoredByLength) {
    Histogram<std::string> hist({"", "aa", "aaaaa", "aaaaaaaaa"});
    for (int64_t i = 0; i < 12; ++i)
        hist.increment(std::string(i, 'a'));
    std::vector<BucketSpec<std::string>> expected = {{0, {}, ""s},
                                                     {2, ""s, "aa"s},
                                                     {3, "aa"s, "aaaaa"s},
                                                     {4, "aaaaa"s, "aaaaaaaaa"s},
                                                     {3, "aaaaaaaaa"s, {}}};

    ASSERT_THAT(snapshot(hist), Eq(expected));
}

TEST_F(HistogramTest, StringCountsIncrementedAndStoredByChar) {
    Histogram<std::string> hist({"a", "h", "r", "z"});
    for (char c = 'a'; c < 'a' + 25; ++c) {
        hist.increment(std::string{c});
    }
    std::vector<BucketSpec<std::string>> expected = {
        {0, {}, "a"s}, {7, "a"s, "h"s}, {10, "h"s, "r"s}, {8, "r"s, "z"s}, {0, "z"s, {}}};

    ASSERT_THAT(snapshot(hist), Eq(expected));
}

TEST_F(HistogramTest, SizeTCountsIncrementedAndStored) {
    Histogram<size_t> hist({0, 5, 8, 12});
    for (size_t i = 0; i < 15; ++i)
        hist.increment(i);
    std::vector<BucketSpec<size_t>> expected = {
        {0, {}, 0}, {5, 0, 5}, {3, 5, 8}, {4, 8, 12}, {3, 12, {}}};

    ASSERT_THAT(snapshot(hist), Eq(expected));
}

}  // namespace
}  // namespace mongo
