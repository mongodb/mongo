// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>


namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * Generic histogram that supports data collection into intervals based on user-specified partitions
 * over any continuous type. A binary predicate that establishes a strict weak ordering over the
 * template parameter type `T` may be specified, otherwise `std::less<T>` is used. (read
 * more here: https://en.cppreference.com/w/cpp/named_req/Compare).
 *
 * For some provided lowermost partition x and uppermost partition y, a value z will be counted
 * in the (-inf, x) interval if z < x, and in the [y, inf) interval if z >= y. If no partitions are
 * provided, z will be counted in the sole (-inf, inf) interval.
 */
template <typename T,
          typename Cmp = std::less<T>,
          typename Counter = std::atomic_int64_t>  // NOLINT
class [[MONGO_MOD_UNFORTUNATELY_OPEN]] Histogram {
    struct AtEnd {};

public:
    explicit Histogram(std::vector<T> partitions, Cmp comparator = {})
        : _partitions{std::move(partitions)},
          _counts(_partitions.size() + 1),
          _comparator{std::move(comparator)} {

        auto ordered =
            std::adjacent_find(_partitions.begin(), _partitions.end(), [&](const T& a, const T& b) {
                return !_comparator(a, b);
            }) == _partitions.end();
        if (!ordered) {
            iasserted(6101800, "Partitions must be strictly monotonically increasing");
        }
    }

    void incrementN(const T& data, int64_t count) {
        auto i = std::upper_bound(_partitions.begin(), _partitions.end(), data, _comparator) -
            _partitions.begin();

        _counts[i] += count;
    }

    void increment(const T& data) {
        incrementN(data, /*count=*/1);
    }

    const std::vector<T>& getPartitions() const {
        return _partitions;
    }

    std::vector<int64_t> getCounts() const {
        std::vector<int64_t> r(_counts.size());
        std::transform(
            _counts.begin(), _counts.end(), r.begin(), [](auto&& x) -> int64_t { return x; });
        return r;
    }

    /**
     * An input iterator over the Histogram class that provides access to histogram buckets, each
     * containing the count, lower and upper bound values. The `lower` data member set to nullptr
     * signifies the lowermost extremity of the distribution. nullptr similarly represents the
     * uppermost extremity when assigned to the `upper` data member.
     */
    class iterator {
    public:
        struct Bucket {
            int64_t count;
            const T* lower;
            const T* upper;
        };

        using difference_type = void;
        using value_type = Bucket;
        using pointer = const Bucket*;
        using reference = const Bucket&;
        using iterator_category = std::input_iterator_tag;

        explicit iterator(const Histogram* hist) : _h{hist}, _pos{0} {}
        iterator(const Histogram* hist, AtEnd) : _h{hist}, _pos{_h->_counts.size()} {}

        reference operator*() const {
            _b.count = _h->_counts[_pos];
            _b.lower = (_pos == 0) ? nullptr : &_h->_partitions[_pos - 1];
            _b.upper = (_pos == _h->_counts.size() - 1) ? nullptr : &_h->_partitions[_pos];
            return _b;
        }

        pointer operator->() const {
            return &**this;
        }

        iterator& operator++() {
            ++_pos;
            return *this;
        }

        iterator operator++(int) {
            iterator orig = *this;
            ++*this;
            return orig;
        }

        friend bool operator==(const iterator& a, const iterator& b) {
            return a._pos == b._pos;
        }

        friend bool operator!=(const iterator& a, const iterator& b) {
            return !(a == b);
        }

    private:
        const Histogram* _h;
        size_t _pos;  // position into _h->_counts
        mutable Bucket _b;
    };

    iterator begin() const {
        return iterator(this);
    }

    iterator end() const {
        return iterator(this, AtEnd{});
    }

protected:
    std::vector<T> _partitions;
    std::vector<Counter> _counts;
    Cmp _comparator;
};

/**
 * Builds the BSON field name ("[lower, upper)") for each bucket of `hist`, in bucket order.
 *
 * The keys depend only on the histogram's partitions, which are fixed at construction. Callers that
 * serialize the same histogram repeatedly should build this once and pass it to the overload of
 * `appendHistogram` below rather than paying the formatting cost on every call.
 */
template <typename... Ts>
std::vector<std::string> makeHistogramBucketKeys(const Histogram<Ts...>& hist) {
    std::vector<std::string> keys;
    keys.reserve(hist.getPartitions().size() + 1);
    for (auto&& bucket : hist) {
        keys.push_back(
            fmt::format("{}{}, {})",
                        bucket.lower ? '[' : '(',
                        bucket.lower ? fmt::to_string(*bucket.lower) : std::string{"-inf"},
                        bucket.upper ? fmt::to_string(*bucket.upper) : std::string{"inf"}));
    }
    return keys;
}

struct AppendHistogramOptions {
    /**
     * When false, buckets with a zero count are omitted. This makes the set of emitted fields vary
     * over time, so only use it for metrics whose consumers tolerate a sparse shape.
     */
    bool includeEmptyBuckets = true;
};

/**
 * Appends data (i.e. count and lower/upper bounds of all buckets) of a histogram to the provided
 * BSON object builder, using bucket keys previously built by `makeHistogramBucketKeys(hist)`.
 * `histKey` is used as the field name for the appended BSON object containing the data.
 *
 * `bucketKeys` must contain exactly one entry per bucket, in bucket order.
 */
template <typename... Ts>
void appendHistogram(BSONObjBuilder& bob,
                     const Histogram<Ts...>& hist,
                     const std::string_view histKey,
                     const std::vector<std::string>& bucketKeys,
                     AppendHistogramOptions options = {}) {
    BSONObjBuilder histBob(bob.subobjStart(histKey));
    long long totalCount = 0;
    size_t i = 0;

    invariant(bucketKeys.size() == hist.getPartitions().size() + 1,
              "bucketKeys must have one entry per histogram bucket");
    for (auto&& bucket : hist) {
        if (bucket.count != 0 || options.includeEmptyBuckets) {
            BSONObjBuilder(histBob.subobjStart(bucketKeys[i]))
                .append("count", static_cast<long long>(bucket.count));
        }
        totalCount += bucket.count;
        ++i;
    }
    histBob.append("totalCount", totalCount);
}

/**
 * As above, but builds the bucket keys on each call. Prefer the overload taking precomputed keys on
 * any path that serializes the same histogram more than once.
 */
template <typename... Ts>
void appendHistogram(BSONObjBuilder& bob,
                     const Histogram<Ts...>& hist,
                     const std::string_view histKey) {
    appendHistogram(bob, hist, histKey, makeHistogramBucketKeys(hist));
}

}  // namespace mongo
