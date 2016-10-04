// update_index_data.h

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

#pragma once

#include <set>

#include "mongo/base/string_data.h"

namespace mongo {

/**
 * a.$ -> a
 * @return true if out is set and we made a change
 */
bool getCanonicalIndexField(StringData fullName, std::string* out);


/**
 * Holds pre-processed index spec information to allow update to quickly determine if an update
 * can be applied as a delta to a document, or if the document must be re-indexed.
 */
class UpdateIndexData {
public:
    UpdateIndexData();

    /**
     * Register a path.  Any update targeting this path (or a parent of this path) will
     * trigger a recomputation of the document's index keys.
     */
    void addPath(StringData path);

    /**
     * Register a path component.  Any update targeting a path that contains this exact
     * component will trigger a recomputation of the document's index keys.
     */
    void addPathComponent(StringData pathComponent);

    /**
     * Register the "wildcard" path.  All updates will trigger a recomputation of the document's
     * index keys.
     */
    void allPathsIndexed();

    void clear();

    bool mightBeIndexed(StringData path) const;

private:
    bool _startsWith(StringData a, StringData b) const;

    std::set<std::string> _canonicalPaths;
    std::set<std::string> _pathComponents;

    bool _allPathsIndexed;
};
}
