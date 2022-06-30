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

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/util/histogram.h"

namespace mongo {

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
    HistogramServerStatusMetric(std::string name, std::vector<uint64_t> bounds)
        : _hist{std::move(bounds)},
          _metric(addMetricToTree(std::make_unique<Metric>(std::move(name), this))) {}

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

private:
    class Metric : public ServerStatusMetric {
    public:
        Metric(std::string name, const HistogramServerStatusMetric* owner)
            : ServerStatusMetric{std::move(name)}, _owner{owner} {}

        void appendAtLeaf(BSONObjBuilder& bob) const override {
            _owner->_appendTo(_leafName, bob);
        }

    private:
        const HistogramServerStatusMetric* const _owner;
    };

    void _appendTo(StringData leafName, BSONObjBuilder& bob) const {
        BSONArrayBuilder arr{bob.subarrayStart(leafName)};
        for (auto&& [count, lower, upper] : _hist)
            BSONObjBuilder(arr.subobjStart())
                .append("lowerBound", static_cast<long long>(lower ? *lower : 0))
                .append("count", count);
    }

    Histogram<uint64_t> _hist;
    Metric& _metric;
};

}  // namespace mongo
