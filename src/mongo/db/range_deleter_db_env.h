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
 */

#pragma once

#include "mongo/db/range_deleter.h"

namespace mongo {

    /**
     * This class implements the deleter methods to be used for a shard.
     */
    struct RangeDeleterDBEnv : public RangeDeleterEnv {
        /**
         * Deletes the documents from the given range synchronously.
         *
         * The keyPattern will be used to determine the right index to use to perform
         * the deletion and it can be a prefix of an existing index. Caller is responsible
         * of making sure that both inclusiveLower and exclusiveUpper is a prefix of keyPattern.
         *
         * Note that secondaryThrottle will be ignored if current process is not part
         * of a replica set.
         *
         * Does not throw Exceptions.
         */
        virtual bool deleteRange(const StringData& ns,
                                 const BSONObj& inclusiveLower,
                                 const BSONObj& exclusiveUpper,
                                 const BSONObj& keyPattern,
                                 bool secondaryThrottle,
                                 std::string* errMsg);

        /**
         * Gets the list of open cursors on a given namespace.
         */
        virtual void getCursorIds(const StringData& ns, std::set<CursorId>* openCursors);
    };
}
