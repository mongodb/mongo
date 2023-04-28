/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include <vector>

#include "mongo/db/exec/sbe/makeobj_enums.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/util/indexed_string_vector.h"

namespace mongo::sbe {
/**
 * MakeObjSpec is a wrapper around a FieldBehavior value and a list of field names / project names.
 */
struct MakeObjSpec {
    using FieldBehavior = MakeObjFieldBehavior;

    static IndexedStringVector buildIndexedFieldVector(std::vector<std::string> fields,
                                                       std::vector<std::string> projects);

    MakeObjSpec(FieldBehavior fieldBehavior,
                std::vector<std::string> fields,
                std::vector<std::string> projects)
        : fieldBehavior(fieldBehavior),
          numKeepOrDrops(fields.size()),
          fieldNames(buildIndexedFieldVector(std::move(fields), std::move(projects))) {}

    std::string toString() const {
        StringBuilder builder;
        builder << (fieldBehavior == MakeObjSpec::FieldBehavior::keep ? "keep" : "drop") << ", [";

        for (size_t i = 0; i < fieldNames.size(); ++i) {
            if (i == numKeepOrDrops) {
                builder << "], [";
            } else if (i != 0) {
                builder << ", ";
            }

            builder << '"' << fieldNames[i] << '"';
        }

        if (fieldNames.size() == numKeepOrDrops) {
            builder << "], [";
        }

        builder << "]";

        return builder.str();
    }

    size_t getApproximateSize() const;

    FieldBehavior fieldBehavior;
    size_t numKeepOrDrops = 0;
    IndexedStringVector fieldNames;
};
}  // namespace mongo::sbe
