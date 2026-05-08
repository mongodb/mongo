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

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/query/query_knob.h"

#include <span>
#include <vector>

namespace mongo::query_settings {

/**
 * Pre-parsed representation of per-query knob overrides stored in QuerySettings.
 *
 * Each entry is a (registry-index, value) pair produced by a single fromBSON() call.
 * An empty entries vector means no overrides are active.
 *
 * DeleteQueryKnobOverride is a write-path-only removal sentinel used to signal that a knob should
 * be removed during merge. It must not survive past merge simplification; toBSON() tasserts its
 * absence.
 */
class QuerySettingsKnobOverrides {
public:
    struct Entry {
        QueryKnobId id;
        QueryKnobValue value;
    };

    static QuerySettingsKnobOverrides fromBSON(const BSONObj& obj);

    BSONObj toBSON() const;

    bool empty() const {
        return _entries.empty();
    }

    std::span<const Entry> entries() const {
        return _entries;
    }

private:
    std::vector<Entry> _entries;
};

}  // namespace mongo::query_settings
