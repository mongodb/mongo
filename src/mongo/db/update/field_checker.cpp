// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/update/field_checker.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/field_ref.h"
#include "mongo/util/str.h"

#include <string_view>

namespace mongo {


namespace fieldchecker {

Status isUpdatable(const FieldRef& field) {
    const size_t numParts = field.numParts();

    if (numParts == 0) {
        return Status(ErrorCodes::EmptyFieldName, "An empty update path is not valid.");
    }

    for (size_t i = 0; i != numParts; ++i) {
        const std::string_view part = field.getPart(i);

        if (part.empty()) {
            return Status(ErrorCodes::EmptyFieldName,
                          str::stream() << "The update path '" << field.dottedField()
                                        << "' contains an empty field name, which is not allowed.");
        }
    }

    return Status::OK();
}

bool isPositionalElement(std::string_view field) {
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
        std::string_view fieldPart = fieldRef.getPart(i);
        if (isPositionalElement(fieldPart)) {
            if (*count == 0)
                *pos = i;
            (*count)++;
        }
    }
    return *count > 0;
}

bool isArrayFilterIdentifier(std::string_view field) {
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
