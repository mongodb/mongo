/**
 *    Copyright 2014 MongoDB Inc.
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

#include "mongo/base/status.h"
#include "mongo/db/jsobj.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

/**
 * Confirms that "o" only contains fields whose names are in "begin".."end",
 * and that no field name occurs multiple times.
 *
 * On failure, returns BadValue and a message naming the unexpected field or DuplicateKey and a
 * message naming the repeated field.  "objectName" is included in the message, for reporting
 * purposes.
 */
template <typename Iter>
Status bsonCheckOnlyHasFields(StringData objectName,
                              const BSONObj& o,
                              const Iter& begin,
                              const Iter& end) {
    std::vector<int> occurrences(std::distance(begin, end), 0);
    for (BSONObj::iterator iter(o); iter.more();) {
        const BSONElement e = iter.next();
        const Iter found = std::find(begin, end, e.fieldNameStringData());
        if (found != end) {
            ++occurrences[std::distance(begin, found)];
        } else {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "Unexpected field " << e.fieldName() << " in "
                                        << objectName);
        }
    }
    int i = 0;
    for (Iter curr = begin; curr != end; ++curr, ++i) {
        if (occurrences[i] > 1) {
            return Status(ErrorCodes::DuplicateKey,
                          str::stream() << "Field " << *curr << " appears " << occurrences[i]
                                        << " times in "
                                        << objectName);
        }
    }
    return Status::OK();
}

/**
 * Same as above, but operates over an array of string-ish items, "legals", instead
 * of "begin".."end".
 */
template <typename StringType, int N>
Status bsonCheckOnlyHasFields(StringData objectName,
                              const BSONObj& o,
                              const StringType (&legals)[N]) {
    return bsonCheckOnlyHasFields(objectName, o, &legals[0], legals + N);
}

}  // namespace mongo
