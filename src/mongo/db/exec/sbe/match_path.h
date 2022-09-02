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

#include <ostream>

#include "mongo/db/field_ref.h"

namespace mongo::sbe {

/**
 * The sbe::MatchPath class is used by SBE to represent field paths for MatchExpressions. This class
 * derives from FieldRef and overrides several methods to provide more uniform behavior in the case
 * where the full path is empty ("").
 */
class MatchPath : public FieldRef {
public:
    MatchPath() = default;
    MatchPath(const MatchPath&) = default;
    MatchPath(MatchPath&&) = default;

    MatchPath(const FieldRef& other) : FieldRef(other) {}
    MatchPath(FieldRef&& other) : FieldRef(std::move(other)) {}

    explicit MatchPath(StringData path) : FieldRef(path) {}

    ~MatchPath() {}

    MatchPath& operator=(const FieldRef& other) {
        *static_cast<FieldRef*>(this) = other;
        return *this;
    }

    MatchPath& operator=(FieldRef&& other) {
        *static_cast<FieldRef*>(this) = std::move(other);
        return *this;
    }

    FieldIndex numParts() const {
        auto n = FieldRef::numParts();
        return n ? n : 1;
    }

    StringData getPart(FieldIndex i) const {
        return FieldRef::numParts() == 0 && i == 0 ? ""_sd : FieldRef::getPart(i);
    }

    StringData operator[](int index) const {
        return getPart(index);
    }

    bool equalsDottedField(StringData other) const {
        return FieldRef::numParts() == 0 ? other == ""_sd : FieldRef::equalsDottedField(other);
    }

    StringData dottedField(FieldIndex offsetFromStart = 0) const {
        if (FieldRef::numParts() == 0) {
            return (offsetFromStart == 0) ? ""_sd : StringData();
        }

        return FieldRef::dottedField(offsetFromStart);
    }

    StringData dottedSubstring(FieldIndex startPart, FieldIndex endPart) const {
        if (!FieldRef::numParts()) {
            return (startPart == 0 && endPart == 1) ? ""_sd : StringData();
        }

        return FieldRef::dottedSubstring(startPart, endPart);
    }

    bool isPathComponentEmpty(FieldIndex i) const {
        return getPart(i) == ""_sd;
    }

    bool hasEmptyPathComponents() const {
        for (FieldIndex i = 0; i < numParts(); ++i) {
            if (isPathComponentEmpty(i)) {
                return true;
            }
        }

        return false;
    }
};

inline std::ostream& operator<<(std::ostream& stream, const MatchPath& field) {
    return stream << field.dottedField();
}

}  // namespace mongo::sbe
