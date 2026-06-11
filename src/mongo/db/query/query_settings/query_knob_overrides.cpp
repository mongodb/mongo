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

BSONObj QuerySettingsKnobOverrides::toBSON() const {
    BSONObjBuilder bob;
    const auto& reg = QueryKnobRegistry::instance();
    for (const auto& [id, val] : _entries) {
        tassert(12194502,
                "DeleteQueryKnobOverride must not survive past simplification",
                !std::holds_alternative<DeleteQueryKnobOverride>(val));
        const auto& entry = reg.entry(id);
        entry.toBSON(bob, entry.wireName, val);
    }
    return bob.obj();
}

}  // namespace mongo::query_settings
