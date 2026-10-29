// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_stats/plan_shape_counters/plan_shape_counts.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/query/query_integration_knobs_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/ctype.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/string_map.h"

#include <string>
#include <vector>

namespace mongo::plan_shape_counters {
namespace {

// The names of the three counter category sections in the wire (toBSON/fromBSON) format.
constexpr auto kPatternSection = "patterns";
constexpr auto kNodesSection = "nodes";
constexpr auto kAccessPathsSection = "accessPaths";

template <typename CounterEnum>
const std::vector<std::string>& counterNames() {
    constexpr size_t numCounters = static_cast<size_t>(CounterEnum::kNumCounters);
    static const auto kEnumNames = [] {
        std::vector<std::string> names;
        for (size_t i = 0; i < numCounters; ++i) {
            auto enumName = toStringData(static_cast<CounterEnum>(i));
            tassert(13022401,
                    "Expected enum name to be length > 1 and begin with 'k'",
                    enumName.size() > 1 && enumName[0] == 'k');
            names.push_back(std::string{enumName.substr(1)});
            names[i][0] = ctype::toLower(names[i][0]);
        }
        return names;
    }();
    return kEnumNames;
}

template <typename CounterEnum>
const StringMap<CounterEnum>& countersByName() {
    static const auto kEnumByName = [] {
        StringMap<CounterEnum> map;
        const auto& names = counterNames<CounterEnum>();
        for (size_t i = 0; i < names.size(); ++i) {
            map[names[i]] = static_cast<CounterEnum>(i);
        }
        tassert(13022402,
                "Expected enum map size to be equal to the number of counters",
                map.size() == static_cast<size_t>(CounterEnum::kNumCounters));
        return map;
    }();
    return kEnumByName;
}

template <typename CounterEnum>
void incrementMap(std::map<CounterEnum, int64_t>& counts, CounterEnum counter, int64_t n) {
    if (kDebugBuild || internalQueryStatsErrorsAreCommandFatal.load()) {
        tassert(13022405, "Expected only non-negative increments on counters", n >= 0);
    }
    if (n > 0) {
        counts[counter] += n;
    }
}

template <typename CounterEnum>
BSONObj sectionToBSON(const std::map<CounterEnum, int64_t>& counts) {
    BSONObjBuilder builder;
    for (const auto& [counter, count] : counts) {
        builder.append(toCounterName(counter), count);
    }
    return builder.obj();
}

template <typename CounterEnum>
void appendSection(BSONObjBuilder& builder,
                   std::string_view sectionName,
                   const std::map<CounterEnum, int64_t>& counts) {
    if (!counts.empty()) {
        builder.append(sectionName, sectionToBSON(counts));
    }
}

template <typename CounterEnum>
void parseSection(const BSONObj& section,
                  std::map<CounterEnum, int64_t>& counts,
                  const StringMap<CounterEnum>& strNameToCounter) {
    for (auto&& elem : section) {
        // Can only parse number counters.
        if (!elem.isNumber() && (kDebugBuild || internalQueryStatsErrorsAreCommandFatal.load())) {
            tasserted(13022403, "Expected number type for counter");
        } else if (!elem.isNumber()) {
            continue;
        }
        auto it = strNameToCounter.find(elem.fieldNameStringData());
        // This lookup may fail if plan shape counters have changed between versions, and we
        // have a cluster that has different versions. Only parse the counters this node supports.
        if (it == strNameToCounter.end()) {
            continue;
        }
        incrementMap(counts, it->second, elem.safeNumberLong());
    }
}
}  // namespace

template <typename CounterEnum>
std::string_view toCounterName(CounterEnum counter) {
    return counterNames<CounterEnum>()[static_cast<size_t>(counter)];
}

template std::string_view toCounterName(PlanShapeCounter);
template std::string_view toCounterName(QsnNodeCounter);
template std::string_view toCounterName(AccessPathCounter);

void PlanShapeCounts::increment(PlanShapeCounter counter, int64_t n) {
    incrementMap(_patternCounts, counter, n);
}

void PlanShapeCounts::increment(QsnNodeCounter counter, int64_t n) {
    incrementMap(_nodeCounts, counter, n);
}

void PlanShapeCounts::increment(AccessPathCounter counter, int64_t n) {
    incrementMap(_accessPathCounts, counter, n);
}

void PlanShapeCounts::add(const PlanShapeCounts& other) {
    for (const auto& [counter, count] : other._patternCounts) {
        increment(counter, count);
    }
    for (const auto& [counter, count] : other._nodeCounts) {
        increment(counter, count);
    }
    for (const auto& [counter, count] : other._accessPathCounts) {
        increment(counter, count);
    }
}

BSONObj PlanShapeCounts::toBSON() const {
    BSONObjBuilder builder;
    appendSection(builder, kPatternSection, _patternCounts);
    appendSection(builder, kNodesSection, _nodeCounts);
    appendSection(builder, kAccessPathsSection, _accessPathCounts);
    return builder.obj();
}

void PlanShapeCounts::parsePatternCounts(const BSONObj& obj) {
    parseSection(obj, _patternCounts, countersByName<PlanShapeCounter>());
}

void PlanShapeCounts::parseNodeCounts(const BSONObj& obj) {
    parseSection(obj, _nodeCounts, countersByName<QsnNodeCounter>());
}

void PlanShapeCounts::parseAccessPathCounts(const BSONObj& obj) {
    parseSection(obj, _accessPathCounts, countersByName<AccessPathCounter>());
}

PlanShapeCounts PlanShapeCounts::fromBSON(const BSONObj& obj) {
    PlanShapeCounts counts;
    for (auto&& elem : obj) {
        // Skip any section that is not an object, since we do not know how to parse it.
        if (elem.type() != BSONType::object) {
            continue;
        }
        auto sectionName = elem.fieldNameStringData();
        // Unrecognized section names are skipped, so nodes on different versions can exchange
        // counts safely.
        if (sectionName == kPatternSection) {
            counts.parsePatternCounts(elem.Obj());
        } else if (sectionName == kNodesSection) {
            counts.parseNodeCounts(elem.Obj());
        } else if (sectionName == kAccessPathsSection) {
            counts.parseAccessPathCounts(elem.Obj());
        }
    }
    return counts;
}

}  // namespace mongo::plan_shape_counters
