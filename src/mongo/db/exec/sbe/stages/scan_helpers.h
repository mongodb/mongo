/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/util/string_listset.h"

namespace mongo {
namespace sbe {
MONGO_COMPILER_ALWAYS_INLINE inline value::OwnedValueAccessor* getFieldAccessor(
    const StringListSet& scanFieldNames,
    absl::InlinedVector<value::OwnedValueAccessor, 4>& scanFieldAccessors,
    StringData name) {
    if (size_t pos = scanFieldNames.findPos(name); pos != StringListSet::npos) {
        return &scanFieldAccessors[pos];
    }
    return nullptr;
}

/*
 * Extract the specified scan fields from record, and place them in the corresponding slot
 * accessors.
 */
MONGO_COMPILER_ALWAYS_INLINE inline void placeFieldsFromRecordInAccessors(
    const Record& record,
    const StringListSet& scanFieldNames,
    absl::InlinedVector<value::OwnedValueAccessor, 4>& scanFieldAccessors) {
    auto rawBson = record.data.data();
    auto start = rawBson + 4;
    auto end = rawBson + ConstDataView(rawBson).read<LittleEndian<uint32_t>>();
    auto last = end - 1;

    if (scanFieldAccessors.size() == 1) {
        // If we're only looking for 1 field, then it's more efficient to forgo the hashtable
        // and just use equality comparison.
        auto name = StringData{scanFieldNames[0]};
        auto [tag, val] = bson::getField(rawBson, name);
        scanFieldAccessors.front().reset(false, tag, val);
    } else {
        // If we're looking for 2 or more fields, it's more efficient to use the hashtable.
        for (auto& accessor : scanFieldAccessors) {
            accessor.reset();
        }

        auto fieldsToMatch = scanFieldAccessors.size();
        for (auto bsonElement = start; bsonElement != last;) {
            auto field = bson::fieldNameAndLength(bsonElement);
            auto accessor = getFieldAccessor(scanFieldNames, scanFieldAccessors, field);

            if (accessor != nullptr) {
                auto [tag, val] = bson::convertFrom<true>(bsonElement, end, field.size());
                accessor->reset(false, tag, val);
                if ((--fieldsToMatch) == 0) {
                    // No need to scan any further so bail out early.
                    break;
                }
            }
            bsonElement = bson::advance(bsonElement, field.size());
        }
    }
}
}  // namespace sbe
}  // namespace mongo
