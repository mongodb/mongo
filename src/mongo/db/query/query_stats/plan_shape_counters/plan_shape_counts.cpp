// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
