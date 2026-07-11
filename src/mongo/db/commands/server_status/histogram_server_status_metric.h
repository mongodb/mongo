// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands/server_status/server_status_metric.h"
#include "mongo/util/histogram.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * Records samples, exposing the aggregate statistics in Server Status as a
 * Histogram.
 *
 * The histogram is represented as a BSON array of bucket BSON
 * objects.
 *
 * Like all ServerStatusMetric counters, instances of this class should
 * be static duration objects at namespace scope.
 */
class HistogramServerStatusMetric {
public:
    explicit HistogramServerStatusMetric(std::vector<uint64_t> bounds) : _hist{std::move(bounds)} {}

    void increment(uint64_t value) {
        _hist.increment(value);
    }

    /**
     * Returns an exponential sequence of `sz` elements. Starts at `base`
     * and increase by a ratio of `rate` between successive elements.
     * This is used to make a `bounds` argument for the constructor.
     */
    static std::vector<uint64_t> pow(size_t sz, uint64_t base, uint64_t rate) {
        std::vector<uint64_t> v;
        v.reserve(sz);
        for (; sz--; base *= rate)
            v.push_back(base);
        return v;
    }

    const Histogram<uint64_t>& hist() const {
        return _hist;
    }

private:
    Histogram<uint64_t> _hist;
};

template <>
struct ServerStatusMetricPolicySelection<HistogramServerStatusMetric> {
    class Policy {
    public:
        explicit Policy(std::vector<uint64_t> bounds) : _v{std::move(bounds)} {}

        auto& value() {
            return _v;
        }

        void appendTo(BSONObjBuilder& bob, std::string_view leafName) const {
            BSONArrayBuilder arr{bob.subarrayStart(leafName)};
            for (auto&& [count, lower, upper] : _v.hist())
                BSONObjBuilder{arr.subobjStart()}
                    .append("lowerBound", static_cast<long long>(lower ? *lower : 0))
                    .append("count", count);
        }

    private:
        HistogramServerStatusMetric _v;
    };
    using type = Policy;
};

}  // namespace mongo
