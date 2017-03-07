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

#include "mongo/base/disallow_copying.h"
#include "mongo/db/namespace_string.h"
#include "mongo/s/catalog/type_chunk.h"

namespace mongo {

class BSONObj;
class Collection;
class OperationContext;

class CollectionRangeDeleter {
    MONGO_DISALLOW_COPYING(CollectionRangeDeleter);

public:
    CollectionRangeDeleter(NamespaceString nss);

    /**
     * Starts deleting ranges and cleans up this object when it is finished.
     */
    void run();

    /**
     * Acquires the collection IX lock and checks whether there are new entries for the collection's
     * rangesToClean structure.  If there are, deletes up to maxToDelete entries and yields using
     * the standard query yielding logic.
     *
     * Returns true if there are more entries in rangesToClean, false if there is no more progress
     * to be made.
     */
    bool cleanupNextRange(OperationContext* opCtx, int maxToDelete);

private:
    /**
     * Performs the deletion of up to maxToDelete entries within the range in progress.
     * This function will invariant if called while _rangeInProgress is not set.
     *
     * Returns the number of documents deleted (0 if deletion is finished), or -1 for error.
     */
    int _doDeletion(OperationContext* opCtx,
                    Collection* collection,
                    const BSONObj& keyPattern,
                    int maxToDelete);

    NamespaceString _nss;

    // Holds a range for which deletion has begun. If empty, then a new range
    // must be requested from rangesToClean
    boost::optional<ChunkRange> _rangeInProgress;
};

}  // namespace mongo
