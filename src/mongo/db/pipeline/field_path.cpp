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
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using std::ostream;
using std::string;
using std::stringstream;
using std::vector;

using namespace mongoutils;

const char FieldPath::prefix[] = "$";

std::string FieldPath::getFullyQualifiedPath(StringData prefix, StringData suffix) {
    if (prefix.empty()) {
        return suffix.toString();
    }
    return str::stream() << prefix << "." << suffix;
}

FieldPath::FieldPath(const vector<string>& fieldNames) {
    massert(16409, "FieldPath cannot be constructed from an empty vector.", !fieldNames.empty());
    _fieldNames.reserve(fieldNames.size());
    for (auto fieldName : fieldNames) {
        pushFieldName(fieldName);
    }
}

FieldPath::FieldPath(const string& fieldPath) {
    // Split 'fieldPath' at the dots.
    size_t startpos = 0;
    while (true) {
        // Find the next dot.
        const size_t dotpos = fieldPath.find('.', startpos);

        // If there are no more dots, use the remainder of the string.
        if (dotpos == fieldPath.npos) {
            string lastFieldName = fieldPath.substr(startpos, dotpos);
            pushFieldName(lastFieldName);
            break;
        }

        // Use the string up to the dot.
        const size_t length = dotpos - startpos;
        string nextFieldName = fieldPath.substr(startpos, length);
        pushFieldName(nextFieldName);

        // Start the next search after the dot.
        startpos = dotpos + 1;
    }
    verify(getPathLength() > 0);
}

string FieldPath::fullPath() const {
    stringstream ss;
    const bool includePrefix = false;
    writePath(ss, includePrefix);
    return ss.str();
}

string FieldPath::fullPathWithPrefix() const {
    stringstream ss;
    const bool includePrefix = true;
    writePath(ss, includePrefix);
    return ss.str();
}

void FieldPath::writePath(ostream& outStream, bool includePrefix) const {
    if (includePrefix)
        outStream << prefix;

    const size_t n = _fieldNames.size();

    verify(n > 0);
    outStream << _fieldNames[0];
    for (size_t i = 1; i < n; ++i)
        outStream << '.' << _fieldNames[i];
}

FieldPath FieldPath::tail() const {
    vector<string> allButFirst(_fieldNames.begin() + 1, _fieldNames.end());
    return FieldPath(allButFirst);
}

void FieldPath::uassertValidFieldName(StringData fieldName) {
    uassert(15998, "FieldPath field names may not be empty strings.", !fieldName.empty());
    uassert(16410, "FieldPath field names may not start with '$'.", fieldName[0] != '$');
    uassert(
        16411, "FieldPath field names may not contain '\0'.", fieldName.find('\0') == string::npos);
    uassert(
        16412, "FieldPath field names may not contain '.'.", fieldName.find('.') == string::npos);
}

void FieldPath::pushFieldName(const string& fieldName) {
    uassertValidFieldName(fieldName);
    _fieldNames.push_back(fieldName);
}
}
