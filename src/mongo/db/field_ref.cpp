/**
 *    Copyright (C) 2012 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/field_ref.h"

#include <algorithm>  // for min

#include "mongo/util/assert_util.h"

namespace mongo {

FieldRef::FieldRef() : _size(0) {}

FieldRef::FieldRef(StringData path) : _size(0) {
    parse(path);
}

void FieldRef::parse(StringData path) {
    if (path.size() == 0) {
        return;
    }

    if (_size != 0) {
        clear();
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

        if (cur != beg)
            appendPart(StringData(&*beg, cur - beg));
        else
            appendPart(StringData());

        if (cur != end) {
            beg = ++cur;
            continue;
        }

        break;
    }
}

void FieldRef::setPart(size_t i, StringData part) {
    dassert(i < _size);

    if (_replacements.size() != _size) {
        _replacements.resize(_size);
    }

    _replacements[i] = part.toString();
    if (i < kReserveAhead) {
        _fixed[i] = _replacements[i];
    } else {
        _variable[getIndex(i)] = _replacements[i];
    }
}

size_t FieldRef::appendPart(StringData part) {
    if (_size < kReserveAhead) {
        _fixed[_size] = part;
    } else {
        _variable.push_back(part);
    }
    return ++_size;
}

void FieldRef::reserialize() const {
    std::string nextDotted;
    // Reserve some space in the string. We know we will have, at minimum, a character for
    // each component we are writing, and a dot for each component, less one. We don't want
    // to reserve more, since we don't want to forfeit the SSO if it is applicable.
    nextDotted.reserve((_size * 2) - 1);

    // Concatenate the fields to a new string
    for (size_t i = 0; i != _size; ++i) {
        if (i > 0)
            nextDotted.append(1, '.');
        const StringData part = getPart(i);
        nextDotted.append(part.rawData(), part.size());
    }

    // Make the new string our contents
    _dotted.swap(nextDotted);

    // Fixup the parts to refer to the new string
    std::string::const_iterator where = _dotted.begin();
    const std::string::const_iterator end = _dotted.end();
    for (size_t i = 0; i != _size; ++i) {
        StringData& part = (i < kReserveAhead) ? _fixed[i] : _variable[getIndex(i)];
        const size_t size = part.size();
        part = StringData(&*where, size);
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

StringData FieldRef::getPart(size_t i) const {
    dassert(i < _size);

    if (i < kReserveAhead) {
        return _fixed[i];
    } else {
        return _variable[getIndex(i)];
    }
}

bool FieldRef::isPrefixOf(const FieldRef& other) const {
    // Can't be a prefix if the size is equal to or larger.
    if (_size >= other._size) {
        return false;
    }

    // Empty FieldRef is not a prefix of anything.
    if (_size == 0) {
        return false;
    }

    size_t common = commonPrefixSize(other);
    return common == _size && other._size > common;
}

size_t FieldRef::commonPrefixSize(const FieldRef& other) const {
    if (_size == 0 || other._size == 0) {
        return 0;
    }

    size_t maxPrefixSize = std::min(_size - 1, other._size - 1);
    size_t prefixSize = 0;

    while (prefixSize <= maxPrefixSize) {
        if (getPart(prefixSize) != other.getPart(prefixSize)) {
            break;
        }
        prefixSize++;
    }

    return prefixSize;
}

StringData FieldRef::dottedField(size_t offset) const {
    return dottedSubstring(offset, numParts());
}

StringData FieldRef::dottedSubstring(size_t startPart, size_t endPart) const {
    if (_size == 0 || startPart >= endPart || endPart > numParts())
        return StringData();

    if (!_replacements.empty())
        reserialize();
    dassert(_replacements.empty());

    StringData result(_dotted);

    // Fast-path if we want the whole thing
    if (startPart == 0 && endPart == numParts())
        return result;

    size_t startChar = 0;
    for (size_t i = 0; i < startPart; ++i) {
        startChar += getPart(i).size() + 1;  // correct for '.'
    }
    size_t endChar = startChar;
    for (size_t i = startPart; i < endPart; ++i) {
        endChar += getPart(i).size() + 1;
    }
    // correct for last '.'
    if (endPart != numParts())
        --endChar;

    return result.substr(startChar, endChar - startChar);
}

bool FieldRef::equalsDottedField(StringData other) const {
    StringData rest = other;

    for (size_t i = 0; i < _size; i++) {
        StringData part = getPart(i);

        if (!rest.startsWith(part))
            return false;

        if (i == _size - 1)
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
    const size_t toCompare = std::min(_size, other._size);
    for (size_t i = 0; i < toCompare; i++) {
        if (getPart(i) == other.getPart(i)) {
            continue;
        }
        return getPart(i) < other.getPart(i) ? -1 : 1;
    }

    const size_t rest = _size - toCompare;
    const size_t otherRest = other._size - toCompare;
    if ((rest == 0) && (otherRest == 0)) {
        return 0;
    } else if (rest < otherRest) {
        return -1;
    } else {
        return 1;
    }
}

void FieldRef::clear() {
    _size = 0;
    _variable.clear();
    _dotted.clear();
    _replacements.clear();
}

std::ostream& operator<<(std::ostream& stream, const FieldRef& field) {
    return stream << field.dottedField();
}

}  // namespace mongo
