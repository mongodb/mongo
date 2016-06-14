/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/bson/bsonobj.h"

#include "mongo/db/repl/optime.h"

namespace mongo {
namespace repl {

/**
 * A parsed oplog entry.
 *
 * This only includes the fields used by the code using this object at the time this was
 * written. As more code uses this, more fields should be added.
 *
 * All unowned members (such as StringDatas and BSONElements) point into the raw BSON.
 * All StringData members are guaranteed to be NUL terminated.
 */
struct OplogEntry {
    // Current oplog version, should be the value of the v field in all oplog entries.
    static const int kOplogVersion;

    explicit OplogEntry(BSONObj raw);

    // This member is not parsed from the BSON and is instead populated by fillWriterVectors.
    bool isForCappedCollection = false;

    bool isCommand() const;
    bool isCrudOpType() const;
    bool hasNamespace() const;
    int getVersion() const;
    BSONElement getIdElement() const;
    OpTime getOpTime() const;
    Seconds getTimestampSecs() const;
    StringData getCollectionName() const;
    std::string toString() const;

    BSONObj raw;  // Owned.

    StringData ns = "";
    StringData opType = "";

    BSONElement version;
    BSONElement o;
    BSONElement o2;
    BSONElement ts;
};

std::ostream& operator<<(std::ostream& s, const OplogEntry& o);

inline bool operator==(const OplogEntry& lhs, const OplogEntry& rhs) {
    return lhs.raw == rhs.raw;
}

}  // namespace repl
}  // namespace mongo
