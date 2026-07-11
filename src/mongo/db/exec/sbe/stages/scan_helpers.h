// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/util/modules.h"
#include "mongo/util/string_listset.h"

#include <string_view>

namespace mongo {
namespace sbe {
MONGO_COMPILER_ALWAYS_INLINE inline value::OwnedValueAccessor* getFieldAccessor(
    const StringListSet& scanFieldNames,
    absl::InlinedVector<value::OwnedValueAccessor, 4>& scanFieldAccessors,
    std::string_view name) {
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

    if (scanFieldAccessors.size() == 1) {
        // If we're only looking for 1 field, then it's more efficient to forgo the hashtable
        // and just use equality comparison.
        scanFieldAccessors.front().reset(bson::getField(rawBson, scanFieldNames[0]));
    } else {
        // If we're looking for 2 or more fields, it's more efficient to use the hashtable.
        for (auto& accessor : scanFieldAccessors) {
            accessor.reset();
        }

        auto start = rawBson + 4;
        auto end = rawBson + ConstDataView(rawBson).read<LittleEndian<uint32_t>>();
        auto last = end - 1;
        auto fieldsToMatch = scanFieldAccessors.size();
        for (auto bsonElement = start; bsonElement != last;) {
            auto field = bson::fieldNameAndLength(bsonElement);
            auto accessor = getFieldAccessor(scanFieldNames, scanFieldAccessors, field);

            if (accessor != nullptr) {
                auto [tag, val] = bson::convertToView(bsonElement, end, field.size());
                accessor->reset(value::TagValueView{tag, val});
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
