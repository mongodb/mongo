/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/update/field_checker.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/field_ref.h"
#include "mongo/util/str.h"

namespace mongo {

using str::stream;

namespace fieldchecker {

Status isUpdatable(const FieldRef& field) {
    const size_t numParts = field.numParts();

    if (numParts == 0) {
        return Status(ErrorCodes::EmptyFieldName, "An empty update path is not valid.");
    }

    for (size_t i = 0; i != numParts; ++i) {
        const StringData part = field.getPart(i);

        if (part.empty()) {
            return Status(ErrorCodes::EmptyFieldName,
                          str::stream() << "The update path '" << field.dottedField()
                                        << "' contains an empty field name, which is not allowed.");
        }
    }

    return Status::OK();
}

bool isPositionalElement(StringData field) {
    return field.size() == 1 && field[0] == '$';
}

bool isPositional(const FieldRef& fieldRef, size_t* pos, size_t* count) {
    // 'count' is optional.
    size_t dummy;
    if (count == nullptr) {
        count = &dummy;
    }

    *count = 0;
    size_t size = fieldRef.numParts();
    for (size_t i = 0; i < size; i++) {
        StringData fieldPart = fieldRef.getPart(i);
        if (isPositionalElement(fieldPart)) {
            if (*count == 0)
                *pos = i;
            (*count)++;
        }
    }
    return *count > 0;
}

bool isArrayFilterIdentifier(StringData field) {
    return field.size() >= 3 && field[0] == '$' && field[1] == '[' &&
        field[field.size() - 1] == ']';
}

bool hasArrayFilter(const FieldRef& fieldRef) {
    auto size = fieldRef.numParts();
    for (size_t i = 0; i < size; i++) {
        auto fieldPart = fieldRef.getPart(i);
        if (isArrayFilterIdentifier(fieldPart)) {
            return true;
        }
    }
    return false;
}

}  // namespace fieldchecker
}  // namespace mongo
