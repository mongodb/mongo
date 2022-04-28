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

#include <algorithm>
#include <functional>
#include <utility>
#include <vector>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/assert_util.h"

namespace mongo {

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
template <typename T, typename Cmp = std::less<T>>
class Histogram {
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

    void increment(const T& data) {
        auto i = std::upper_bound(_partitions.begin(), _partitions.end(), data, _comparator) -
            _partitions.begin();

        _counts[i].addAndFetch(1);
    }

    const std::vector<T>& getPartitions() const {
        return _partitions;
    }

    std::vector<int64_t> getCounts() const {
        std::vector<int64_t> r(_counts.size());
        std::transform(
            _counts.begin(), _counts.end(), r.begin(), [](auto&& x) { return x.load(); });
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
            _b.count = _h->_counts[_pos].load();
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

private:
    std::vector<T> _partitions;
    std::vector<AtomicWord<int64_t>> _counts;
    Cmp _comparator;
};

/**
 * Appends data (i.e. count and lower/upper bounds of all buckets) of a histogram to the provided
 * BSON object builder. `histKey` is used as the field name for the appended BSON object containing
 * the data.
 */
template <typename T>
void appendHistogram(BSONObjBuilder& bob, const Histogram<T>& hist, const StringData histKey) {
    BSONObjBuilder histBob(bob.subobjStart(histKey));
    long long totalCount = 0;

    using namespace fmt::literals;
    for (auto&& [count, lower, upper] : hist) {
        std::string bucketKey = "{}{}, {})"_format(lower ? "[" : "(",
                                                   lower ? "{}"_format(*lower) : "-inf",
                                                   upper ? "{}"_format(*upper) : "inf");

        BSONObjBuilder(histBob.subobjStart(bucketKey))
            .append("count", static_cast<long long>(count));
        totalCount += count;
    }
    histBob.append("totalCount", totalCount);
}

}  // namespace mongo
