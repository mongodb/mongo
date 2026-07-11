// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_settings/query_knob_overrides.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/query/query_knobs/query_knob_registry.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <variant>

namespace mongo::query_settings {

QuerySettingsKnobOverrides QuerySettingsKnobOverrides::fromBSON(const BSONObj& obj) {
    QuerySettingsKnobOverrides overrides;
    const auto& reg = QueryKnobRegistry::instance();
    for (auto&& elem : obj) {
        auto id = reg.getKnobIdForName(elem.fieldNameStringData());
        uassert(12194500,
                str::stream() << "query knob not settable via QuerySettings: "
                              << elem.fieldNameStringData(),
                id.has_value());

        if (elem.isNull()) {
            overrides._entries.emplace_back(*id, DeleteQueryKnobOverride{});
            continue;
        }

        const auto& entry = reg.entry(*id);
        try {
            // fromBSON() only type-checks; validate() also enforces IDL range constraints.
            uassertStatusOK(entry.param->validate(elem, boost::none));
            overrides._entries.emplace_back(Entry{*id, entry.fromBSON(elem)});
        } catch (const DBException& ex) {
            uasserted(12194501,
                      str::stream() << "failed to parse query knob " << elem.fieldNameStringData()
                                    << ": " << ex.reason());
        }
    }
    // Sort the entries so order is deterministic. Use a total order: sort by id, then by variant
    // type index, then by value, to handle duplicate ids from BSON with repeated field names.
    std::sort(overrides._entries.begin(), overrides._entries.end());
    return overrides;
}

QuerySettingsKnobOverrides QuerySettingsKnobOverrides::merge(
    const QuerySettingsKnobOverrides& lhs, const QuerySettingsKnobOverrides& rhs) {
    // Both _entries are sorted by (id, value).
    QuerySettingsKnobOverrides result;
    result._entries.reserve(lhs._entries.size() + rhs._entries.size());
    std::set_union(rhs._entries.begin(),
                   rhs._entries.end(),
                   lhs._entries.begin(),
                   lhs._entries.end(),
                   std::back_inserter(result._entries),
                   [](const Entry& a, const Entry& b) { return a.id < b.id; });
    return result;
}

bool QuerySettingsKnobOverrides::removeKnobsRequiringHigherFcv(
    multiversion::FeatureCompatibilityVersion fcv) {
    const auto& reg = QueryKnobRegistry::instance();
    const auto sizeBefore = _entries.size();
    _entries.erase(std::remove_if(_entries.begin(),
                                  _entries.end(),
                                  [&](const Entry& e) { return *reg.entry(e.id).minFcv > fcv; }),
                   _entries.end());
    return _entries.size() != sizeBefore;
}

void QuerySettingsKnobOverrides::simplify() {
    auto isDelete = [](const Entry& e) {
        return std::holds_alternative<DeleteQueryKnobOverride>(e.value);
    };
    _entries.erase(std::remove_if(_entries.begin(), _entries.end(), isDelete), _entries.end());
}

BSONObj QuerySettingsKnobOverrides::toBSON() const {
    BSONObjBuilder bob;
    const auto& reg = QueryKnobRegistry::instance();
    for (const auto& [id, val] : _entries) {
        if (std::holds_alternative<DeleteQueryKnobOverride>(val)) {
            bob.appendNull(reg.entry(id).wireName);
            continue;
        }
        const auto& entry = reg.entry(id);
        entry.toBSON(bob, entry.wireName, val);
    }
    return bob.obj();
}

}  // namespace mongo::query_settings
