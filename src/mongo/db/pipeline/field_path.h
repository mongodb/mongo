/**
 * Copyright (c) 2011 10gen Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects for
 * all of the code used other than as permitted herein. If you modify file(s)
 * with this exception, you may extend this exception to your version of the
 * file(s), but you are not obligated to do so. If you do not wish to do so,
 * delete this exception statement from your version. If you delete this
 * exception statement from all source files in the program, then also delete
 * it in the license file.
 */

#pragma once

#include <iosfwd>
#include <string>
#include <vector>

#include "mongo/util/assert_util.h"

namespace mongo {

class StringData;

/**
 * Utility class which represents a field path with nested paths separated by dots.
 */
class FieldPath {
public:
    /**
     * Throws a UserException if a field name does not pass validation.
     */
    static void uassertValidFieldName(StringData fieldName);

    /**
     * Concatenates 'prefix' and 'suffix' using dotted path notation. 'prefix' is allowed to be
     * empty.
     */
    static std::string getFullyQualifiedPath(StringData prefix, StringData suffix);

    /**
     * Throws a UserException if the string is empty or if any of the field names fail validation.
     *
     * Field names are validated using uassertValidFieldName().
     */
    FieldPath(const std::string& fieldPath);

    /**
     * Throws a UserException if 'fieldNames' is empty or if any of the field names fail validation.
     *
     * Field names are validated using uassertValidFieldName().
     */
    FieldPath(const std::vector<std::string>& fieldNames);

    /**
     * Returns the number of path elements in the field path.
     */
    size_t getPathLength() const {
        return _fieldNames.size();
    }

    /**
     * Return the ith field name from this path using zero-based indexes.
     */
    const std::string& getFieldName(size_t i) const {
        dassert(i < getPathLength());
        return _fieldNames[i];
    }

    /**
     * Returns the full path, not including the prefix 'FieldPath::prefix'.
     */
    std::string fullPath() const;

    /**
     * Returns the full path, including the prefix 'FieldPath::prefix'.
     */
    std::string fullPathWithPrefix() const;

    /**
     * Write the full path to 'outStream', including the prefix 'FieldPath::prefix' if
     * 'includePrefix' is specified.
     */
    void writePath(std::ostream& outStream, bool includePrefix) const;

    static const char* getPrefix() {
        return prefix;
    }

    /**
     * A FieldPath like this but missing the first element (useful for recursion).
     * Precondition getPathLength() > 1.
     */
    FieldPath tail() const;

private:
    /**
     * Push a new field name to the back of the vector of names comprising the field path.
     *
     * Throws a UserException if 'fieldName' does not pass validation done by
     * uassertValidFieldName().
     */
    void pushFieldName(const std::string& fieldName);

    static const char prefix[];

    std::vector<std::string> _fieldNames;
};
}
