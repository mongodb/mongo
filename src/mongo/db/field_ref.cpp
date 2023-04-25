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

#include "mongo/platform/basic.h"

#include "mongo/db/field_ref.h"

#include <algorithm>

#include "mongo/util/assert_util.h"
#include "mongo/util/ctype.h"

namespace mongo {

FieldRef::FieldRef(StringData path) {
    parse(path);
}

void FieldRef::parse(StringData path) {
    clear();

    if (path.size() == 0) {
        return;
    }

    // We guarantee that accesses through getPart() will be valid while 'this' is. So we
    // keep a copy in a local sting.

    _dotted = path.toString();

    // Separate the field parts using '.' as a delimiter.
    std::string::iterator beg = _dotted.begin();
    std::string::iterator cur = beg;
    const std::string::iterator end = _dotted.end();
    while (true) {
        if (cur != end && *cur != '.') {
            cur++;
            continue;
        }

        // If cur != beg then we advanced cur in the loop above, so we have a real sequence
        // of characters to add as a new part. Otherwise, we may be parsing something odd,
        // like "..", and we need to add an empty StringData piece to represent the "part"
        // in-between the dots. This also handles the case where 'beg' and 'cur' are both
        // at 'end', which can happen if we are parsing anything with a terminal "."
        // character. In that case, we still need to add an empty part, but we will break
        // out of the loop below since we will not execute the guarded 'continue' and will
        // instead reach the break statement.

        if (cur != beg) {
            size_t offset = beg - _dotted.begin();
            size_t len = cur - beg;
            appendParsedPart(StringView{offset, len});
        } else {
            appendParsedPart(StringView{});
        }

        if (cur != end) {
            beg = ++cur;
            continue;
        }

        break;
    }
}

void FieldRef::setPart(FieldIndex i, StringData part) {
    dassert(i < _parts.size());

    if (_replacements.empty()) {
        _replacements.resize(_parts.size());
    }

    _replacements[i] = part.toString();
    _parts[i] = boost::none;
}

void FieldRef::appendPart(StringData part) {
    if (_replacements.empty()) {
        _replacements.resize(_parts.size());
    }

    _replacements.push_back(part.toString());
    _parts.push_back(boost::none);
}

void FieldRef::removeLastPart() {
    if (_parts.size() == 0) {
        return;
    }

    if (!_replacements.empty()) {
        _replacements.pop_back();
    }

    _parts.pop_back();
}

void FieldRef::removeFirstPart() {
    if (_parts.size() == 0) {
        return;
    }
    for (size_t i = 0; i + 1 < _parts.size(); ++i) {
        setPart(i, getPart(i + 1));
    }
    removeLastPart();
}

size_t FieldRef::appendParsedPart(FieldRef::StringView part) {
    _parts.push_back(part);
    _cachedSize++;
    return _parts.size();
}

void FieldRef::reserialize() const {
    auto parts = _parts.size();
    std::string nextDotted;
    // Reserve some space in the string. We know we will have, at minimum, a character for
    // each component we are writing, and a dot for each component, less one. We don't want
    // to reserve more, since we don't want to forfeit the SSO if it is applicable.
    nextDotted.reserve((parts > 0) ? (parts * 2) - 1 : 0);

    // Concatenate the fields to a new string
    for (size_t i = 0; i != _parts.size(); ++i) {
        if (i > 0)
            nextDotted.append(1, '.');
        const StringData part = getPart(i);
        nextDotted.append(part.rawData(), part.size());
    }

    // Make the new string our contents
    _dotted.swap(nextDotted);

    // Before we reserialize, it's possible that _cachedSize != _size because parts were added or
    // removed. This reserialization process reconciles the components in our cached string
    // (_dotted) with the modified path.
    _cachedSize = parts;

    // Fixup the parts to refer to the new string
    std::string::const_iterator where = _dotted.begin();
    const std::string::const_iterator end = _dotted.end();
    for (size_t i = 0; i != parts; ++i) {
        boost::optional<StringView>& part = _parts[i];
        const size_t size = part ? part->len : _replacements[i].size();

        // There is one case where we expect to see the "where" iterator to be at "end" here: we
        // are at the last part of the FieldRef and that part is the empty string. In that case, we
        // need to make sure we do not dereference the "where" iterator.
        invariant(where != end || (size == 0 && i == parts - 1));
        if (!size) {
            part = StringView{};
        } else {
            std::size_t offset = where - _dotted.begin();
            part = StringView{offset, size};
        }
        where += size;
        // skip over '.' unless we are at the end.
        if (where != end) {
            dassert(*where == '.');
            ++where;
        }
    }

    // Drop any replacements
    _replacements.clear();
}

StringData FieldRef::getPart(FieldIndex i) const {
    invariant(i < _parts.size());

    const boost::optional<StringView>& part = _parts[i];
    if (part) {
        return part->toStringData(_dotted);
    } else {
        return StringData(_replacements[i]);
    }
}

bool FieldRef::isPrefixOf(const FieldRef& other) const {
    // Can't be a prefix if the size is equal to or larger.
    if (_parts.size() >= other._parts.size()) {
        return false;
    }

    // Empty FieldRef is not a prefix of anything.
    if (_parts.size() == 0) {
        return false;
    }

    size_t common = commonPrefixSize(other);
    return common == _parts.size() && other._parts.size() > common;
}

bool FieldRef::isPrefixOfOrEqualTo(const FieldRef& other) const {
    return isPrefixOf(other) || *this == other;
}

bool FieldRef::fullyOverlapsWith(const FieldRef& other) const {
    auto common = commonPrefixSize(other);
    return common && (common == numParts() || common == other.numParts());
}

FieldIndex FieldRef::commonPrefixSize(const FieldRef& other) const {
    if (_parts.size() == 0 || other._parts.size() == 0) {
        return 0;
    }

    FieldIndex maxPrefixSize = std::min(_parts.size() - 1, other._parts.size() - 1);
    FieldIndex prefixSize = 0;

    while (prefixSize <= maxPrefixSize) {
        if (getPart(prefixSize) != other.getPart(prefixSize)) {
            break;
        }
        prefixSize++;
    }

    return prefixSize;
}

bool FieldRef::isNumericPathComponentStrict(StringData component) {
    return !component.empty() && !(component.size() > 1 && component[0] == '0') &&
        FieldRef::isNumericPathComponentLenient(component);
}

bool FieldRef::isNumericPathComponentLenient(StringData component) {
    return !component.empty() &&
        std::all_of(component.begin(), component.end(), [](auto c) { return ctype::isDigit(c); });
}

bool FieldRef::isNumericPathComponentStrict(FieldIndex i) const {
    return FieldRef::isNumericPathComponentStrict(getPart(i));
}

bool FieldRef::isNumericPathComponentLenient(FieldIndex i) const {
    return FieldRef::isNumericPathComponentLenient(getPart(i));
}

bool FieldRef::hasNumericPathComponents() const {
    for (size_t i = 0; i < numParts(); ++i) {
        if (isNumericPathComponentStrict(i))
            return true;
    }
    return false;
}

std::set<FieldIndex> FieldRef::getNumericPathComponents(FieldIndex startPart) const {
    std::set<FieldIndex> numericPathComponents;
    for (auto i = startPart; i < numParts(); ++i) {
        if (isNumericPathComponentStrict(i))
            numericPathComponents.insert(i);
    }
    return numericPathComponents;
}

StringData FieldRef::dottedField(FieldIndex offset) const {
    return dottedSubstring(offset, numParts());
}

StringData FieldRef::dottedSubstring(FieldIndex startPart, FieldIndex endPart) const {
    if (_parts.size() == 0 || startPart >= endPart || endPart > numParts())
        return StringData();

    if (!_replacements.empty() || _parts.size() != _cachedSize)
        reserialize();
    dassert(_replacements.empty() && _parts.size() == _cachedSize);

    StringData result(_dotted);

    // Fast-path if we want the whole thing
    if (startPart == 0 && endPart == numParts())
        return result;

    size_t startChar = 0;
    for (FieldIndex i = 0; i < startPart; ++i) {
        startChar += getPart(i).size() + 1;  // correct for '.'
    }
    size_t endChar = startChar;
    for (FieldIndex i = startPart; i < endPart; ++i) {
        endChar += getPart(i).size() + 1;
    }
    // correct for last '.'
    if (endPart != numParts())
        --endChar;

    return result.substr(startChar, endChar - startChar);
}

bool FieldRef::equalsDottedField(StringData other) const {
    StringData rest = other;

    for (FieldIndex i = 0; i < _parts.size(); i++) {
        StringData part = getPart(i);

        if (!rest.startsWith(part))
            return false;

        if (i == _parts.size() - 1)
            return rest.size() == part.size();

        // make sure next thing is a dot
        if (rest.size() == part.size())
            return false;

        if (rest[part.size()] != '.')
            return false;

        rest = rest.substr(part.size() + 1);
    }

    return false;
}

int FieldRef::compare(const FieldRef& other) const {
    const FieldIndex toCompare = std::min(_parts.size(), other._parts.size());
    for (FieldIndex i = 0; i < toCompare; i++) {
        if (getPart(i) == other.getPart(i)) {
            continue;
        }
        return getPart(i) < other.getPart(i) ? -1 : 1;
    }

    const FieldIndex rest = _parts.size() - toCompare;
    const FieldIndex otherRest = other._parts.size() - toCompare;
    if ((rest == 0) && (otherRest == 0)) {
        return 0;
    } else if (rest < otherRest) {
        return -1;
    } else {
        return 1;
    }
}

void FieldRef::clear() {
    _cachedSize = 0;
    _parts.clear();
    _dotted.clear();
    _replacements.clear();
}

std::ostream& operator<<(std::ostream& stream, const FieldRef& field) {
    return stream << field.dottedField();
}

}  // namespace mongo
