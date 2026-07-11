// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/util/modules.h"
#include "mongo/util/string_map.h"

#include <vector>


namespace mongo::timeseries::write_ops_utils::details {

/**
 * Helper for measurement sorting.
 * timeField: {"<timeField>": "2022-06-06T15:34:30.000Z"}
 * dataFields: [{"<timefield>": 2022-06-06T15:34:30.000Z}, {"a": 1}, {"b": 2}]
 */
struct Measurement {
    BSONElement timeField;
    std::vector<BSONElement> dataFields;
};

inline bool operator==(const timeseries::write_ops_utils::details::Measurement& lhs,
                       const timeseries::write_ops_utils::details::Measurement& rhs) {
    bool timeFieldEqual = (lhs.timeField.woCompare(rhs.timeField) == 0);
    if (!timeFieldEqual || (lhs.dataFields.size() != rhs.dataFields.size())) {
        return false;
    }

    StringMap<BSONElement> rhsFields;
    for (auto& field : rhs.dataFields) {
        rhsFields.insert({std::string{field.fieldNameStringData()}, field});
    }

    for (size_t i = 0; i < lhs.dataFields.size(); ++i) {
        auto& lhsField = lhs.dataFields[i];
        auto it = rhsFields.find(std::string{lhsField.fieldNameStringData()});
        if (it == rhsFields.end()) {
            return false;
        }

        if (it->second.woCompare(lhsField) != 0) {
            return false;
        }
    }
    return true;
}

}  // namespace mongo::timeseries::write_ops_utils::details
