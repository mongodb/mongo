// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/field_ref.h"
#include "mongo/util/modules.h"

#include <ostream>
#include <string_view>

namespace mongo::sbe {
using namespace std::literals::string_view_literals;

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
    MatchPath(FieldRef&& other) : FieldRef(other) {}

    explicit MatchPath(std::string_view path) : FieldRef(path) {}

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

    std::string_view getPart(FieldIndex i) const {
        return FieldRef::numParts() == 0 && i == 0 ? ""sv : FieldRef::getPart(i);
    }

    std::string_view operator[](int index) const {
        return getPart(index);
    }

    bool equalsDottedField(std::string_view other) const {
        return FieldRef::numParts() == 0 ? other == ""sv : FieldRef::equalsDottedField(other);
    }

    std::string_view dottedField(FieldIndex offsetFromStart = 0) const {
        if (FieldRef::numParts() == 0) {
            return (offsetFromStart == 0) ? ""sv : std::string_view();
        }

        return FieldRef::dottedField(offsetFromStart);
    }

    std::string_view dottedSubstring(FieldIndex startPart, FieldIndex endPart) const {
        if (!FieldRef::numParts()) {
            return (startPart == 0 && endPart == 1) ? ""sv : std::string_view();
        }

        return FieldRef::dottedSubstring(startPart, endPart);
    }

    bool isPathComponentEmpty(FieldIndex i) const {
        return getPart(i) == ""sv;
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
