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

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/field_path.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bson_depth.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using std::string;
using std::vector;

string FieldPath::getFullyQualifiedPath(StringData prefix, StringData suffix) {
    if (prefix.empty()) {
        return suffix.toString();
    }

    return str::stream() << prefix << "." << suffix;
}

FieldPath::FieldPath(std::string inputPath)
    : _fieldPath(std::move(inputPath)), _fieldPathDotPosition{string::npos} {
    uassert(40352, "FieldPath cannot be constructed with empty string", !_fieldPath.empty());
    uassert(40353, "FieldPath must not end with a '.'.", _fieldPath[_fieldPath.size() - 1] != '.');

    // Store index delimiter position for use in field lookup.
    size_t dotPos;
    size_t startPos = 0;
    while (string::npos != (dotPos = _fieldPath.find('.', startPos))) {
        _fieldPathDotPosition.push_back(dotPos);
        startPos = dotPos + 1;
    }

    _fieldPathDotPosition.push_back(_fieldPath.size());

    // Validate the path length and the fields.
    const auto pathLength = getPathLength();
    uassert(ErrorCodes::Overflow,
            "FieldPath is too long",
            pathLength <= BSONDepth::getMaxAllowableDepth());
    for (size_t i = 0; i < pathLength; ++i) {
        uassertValidFieldName(getFieldName(i));
    }
}

void FieldPath::uassertValidFieldName(StringData fieldName) {
    uassert(15998, "FieldPath field names may not be empty strings.", !fieldName.empty());
    uassert(16410, "FieldPath field names may not start with '$'.", fieldName[0] != '$');
    uassert(
        16411, "FieldPath field names may not contain '\0'.", fieldName.find('\0') == string::npos);
    uassert(
        16412, "FieldPath field names may not contain '.'.", fieldName.find('.') == string::npos);
}
}
