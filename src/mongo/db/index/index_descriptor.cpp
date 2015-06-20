// index_descriptor.cpp

/**
*    Copyright (C) 2014 10gen Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kIndex

#include "mongo/db/index/index_descriptor.h"
#include "mongo/util/log.h"

namespace mongo {

namespace {
void populateOptionsMap(std::map<StringData, BSONElement>& theMap, const BSONObj& spec) {
    BSONObjIterator it(spec);
    while (it.more()) {
        const BSONElement e = it.next();

        StringData fieldName = e.fieldNameStringData();
        if (fieldName == "key" || fieldName == "ns" || fieldName == "name" ||
            fieldName == "v" ||                     // not considered for equivalence
            fieldName == "textIndexVersion" ||      // same as "v"
            fieldName == "2dsphereIndexVersion" ||  // same as "v"
            fieldName == "background" ||            // this is a creation time option only
            fieldName == "dropDups" ||              // this is now ignored
            fieldName == "sparse" ||                // checked specially
            fieldName == "unique"                   // check specially
            ) {
            continue;
        }
        theMap[fieldName] = e;
    }
}
}

bool IndexDescriptor::areIndexOptionsEquivalent(const IndexDescriptor* other) const {
    if (isSparse() != other->isSparse()) {
        return false;
    }

    if (!isIdIndex() && unique() != other->unique()) {
        // Note: { _id: 1 } or { _id: -1 } implies unique: true.
        return false;
    }

    // Then compare the rest of the options.

    std::map<StringData, BSONElement> existingOptionsMap;
    populateOptionsMap(existingOptionsMap, infoObj());

    std::map<StringData, BSONElement> newOptionsMap;
    populateOptionsMap(newOptionsMap, other->infoObj());

    return existingOptionsMap == newOptionsMap;
}

void IndexDescriptor::_checkOk() const {
    if (_magic == 123987)
        return;
    log() << "uh oh: " << (void*)(this) << " " << _magic;
    invariant(0);
}
}
