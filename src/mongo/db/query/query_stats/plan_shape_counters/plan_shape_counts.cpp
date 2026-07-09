/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/query/query_stats/plan_shape_counters/plan_shape_counts.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/ctype.h"
#include "mongo/util/static_immortal.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"

#include <array>
#include <string>

namespace mongo::plan_shape_counters {
namespace {

const std::array<std::string, kNumPlanShapeCounters>& counterNames() {
    static const auto kEnumNames = [] {
        std::array<std::string, kNumPlanShapeCounters> names;
        for (size_t i = 0; i < kNumPlanShapeCounters; ++i) {
            auto enumName = toStringData(static_cast<PlanShapeCounter>(i));
            tassert(13022401,
                    "Expected enum name to be length > 1 and begin with 'k'",
                    enumName.size() > 1 && enumName[0] == 'k');
            names[i] = std::string{enumName.substr(1)};
            names[i][0] = ctype::toLower(names[i][0]);
        }
        return names;
    }();
    return kEnumNames;
}

const StringMap<PlanShapeCounter>& countersByName() {
    static const auto kEnumByName = [] {
        StringMap<PlanShapeCounter> map;
        const auto& names = counterNames();
        for (size_t i = 0; i < names.size(); ++i) {
            map[names[i]] = static_cast<PlanShapeCounter>(i);
        }
        tassert(13022402,
                "Expected enum map size to be equal to the number of counters",
                map.size() == kNumPlanShapeCounters);
        return map;
    }();
    return kEnumByName;
}

}  // namespace

std::string_view toCounterName(PlanShapeCounter shape) {
    return counterNames()[static_cast<size_t>(shape)];
}

void PlanShapeCounts::increment(PlanShapeCounter shape, int64_t n) {
    tassert(13022405, "Expected only non-negative increments on counters", n >= 0);
    _counts[shape] += n;
}

void PlanShapeCounts::add(const PlanShapeCounts& other) {
    for (const auto& [shape, count] : other._counts) {
        increment(shape, count);
    }
}

BSONObj PlanShapeCounts::toBSON() const {
    BSONObjBuilder builder;
    for (const auto& [shape, count] : _counts) {
        builder.append(toCounterName(shape), count);
    }
    return builder.obj();
}

PlanShapeCounts PlanShapeCounts::fromBSON(const BSONObj& obj) {
    PlanShapeCounts counts;
    const auto& byName = countersByName();
    for (auto&& elem : obj) {
        tassert(13022400,
                str::stream() << "Expected a numeric type for plan shape counter '"
                              << elem.fieldNameStringData() << "', got " << typeName(elem.type()),
                elem.isNumber());
        auto it = byName.find(elem.fieldNameStringData());
        // This lookup may fail if plan shape counters have changed between versions, and we
        // have a cluster that has different versions. Only parse the counters this node supports.
        if (it == byName.end()) {
            continue;
        }
        counts.increment(it->second, elem.safeNumberLong());
    }
    return counts;
}

}  // namespace mongo::plan_shape_counters
