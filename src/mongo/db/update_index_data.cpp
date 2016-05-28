// update_index_data.cpp

/**
*    Copyright (C) 2013 10gen Inc.
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

#include "mongo/db/update_index_data.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/field_ref.h"

namespace mongo {

using std::string;

UpdateIndexData::UpdateIndexData() : _allPathsIndexed(false) {}

void UpdateIndexData::addPath(StringData path) {
    string s;
    if (getCanonicalIndexField(path, &s)) {
        _canonicalPaths.insert(s);
    } else {
        _canonicalPaths.insert(path.toString());
    }
}

void UpdateIndexData::addPathComponent(StringData pathComponent) {
    _pathComponents.insert(pathComponent.toString());
}

void UpdateIndexData::allPathsIndexed() {
    _allPathsIndexed = true;
}

void UpdateIndexData::clear() {
    _canonicalPaths.clear();
    _pathComponents.clear();
    _allPathsIndexed = false;
}

bool UpdateIndexData::mightBeIndexed(StringData path) const {
    if (_allPathsIndexed) {
        return true;
    }

    StringData use = path;
    string x;
    if (getCanonicalIndexField(path, &x))
        use = StringData(x);

    for (std::set<string>::const_iterator i = _canonicalPaths.begin(); i != _canonicalPaths.end();
         ++i) {
        StringData idx(*i);

        if (_startsWith(use, idx))
            return true;

        if (_startsWith(idx, use))
            return true;
    }

    FieldRef pathFieldRef(path);
    for (std::set<string>::const_iterator i = _pathComponents.begin(); i != _pathComponents.end();
         ++i) {
        const string& pathComponent = *i;
        for (size_t partIdx = 0; partIdx < pathFieldRef.numParts(); ++partIdx) {
            if (pathComponent == pathFieldRef.getPart(partIdx)) {
                return true;
            }
        }
    }

    return false;
}

bool UpdateIndexData::_startsWith(StringData a, StringData b) const {
    if (!a.startsWith(b))
        return false;

    // make sure there is a dot or EOL right after

    if (a.size() == b.size())
        return true;

    return a[b.size()] == '.';
}

bool getCanonicalIndexField(StringData fullName, string* out) {
    // check if fieldName contains ".$" or ".###" substrings (#=digit) and skip them
    // however do not skip the first field even if it meets these criteria

    if (fullName.find('.') == string::npos)
        return false;

    bool modified = false;

    StringBuilder buf;
    for (size_t i = 0; i < fullName.size(); i++) {
        char c = fullName[i];

        if (c != '.') {
            buf << c;
            continue;
        }

        if (i + 1 == fullName.size()) {
            // ends with '.'
            buf << c;
            continue;
        }

        // check for ".$", skip if present
        if (fullName[i + 1] == '$') {
            // only do this if its not something like $a
            if (i + 2 >= fullName.size() || fullName[i + 2] == '.') {
                i++;
                modified = true;
                continue;
            }
        }

        // check for ".###" for any number of digits (no letters)
        if (isdigit(fullName[i + 1])) {
            size_t j = i;
            // skip digits
            while (j + 1 < fullName.size() && isdigit(fullName[j + 1]))
                j++;

            if (j + 1 == fullName.size() || fullName[j + 1] == '.') {
                // only digits found, skip forward
                i = j;
                modified = true;
                continue;
            }
        }

        buf << c;
    }

    if (!modified)
        return false;

    *out = buf.str();
    return true;
}
}
