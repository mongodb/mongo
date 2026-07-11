// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/histogram.h"

#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"

#include <ostream>
#include <tuple>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

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
            auto lens = [](auto&& x) {
                return std::tie(x.count, x.lower, x.upper);
            };
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
