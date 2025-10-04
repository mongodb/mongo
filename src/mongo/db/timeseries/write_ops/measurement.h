/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/bson/bsonelement.h"
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
