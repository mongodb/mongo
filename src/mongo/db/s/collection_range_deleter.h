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
#include "mongo/executor/task_executor.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/util/concurrency/notification.h"

namespace mongo {

class BSONObj;
class Collection;
class OperationContext;

class CollectionRangeDeleter {
    MONGO_DISALLOW_COPYING(CollectionRangeDeleter);

public:
    //
    // All of the following members must be called only while the containing MetadataManager's lock
    // is held (or in its destructor), except cleanUpNextRange.
    //

    /**
     * Normally, construct with the collection name and ShardingState's dedicated executor.
     */
    CollectionRangeDeleter() = default;
    ~CollectionRangeDeleter();

    using DeleteNotification = std::shared_ptr<Notification<Status>>;

    /**
     * Adds a new range to be cleaned up by the cleaner thread.
     */
    void add(const ChunkRange& range);

    /**
     * Reports whether the argument range overlaps any of the ranges to clean.  If there is overlap,
     * it returns a notification that will be completed when the currently newest overlapping
     * range is no longer scheduled.  Its value indicates whether it has been successfully removed.
     * If there is no overlap, the result is nullptr.  After a successful removal, the caller
     * should call again to ensure no other range overlaps the argument.
     * (See CollectionShardingState::waitForClean and MetadataManager::trackOrphanedDataCleanup for
     * an example use.)
     */
    DeleteNotification overlaps(ChunkRange const& range) const;

    /**
     * Reports the number of ranges remaining to be cleaned up.
     */
    size_t size() const;

    bool isEmpty() const;

    /*
     * Notify with the specified status anything waiting on ranges scheduled, before discarding the
     * ranges and notifications.
     */
    void clear(Status);

    /*
     * Append a representation of self to the specified builder.
     */
    void append(BSONObjBuilder* builder) const;

    /**
     * If any ranges are scheduled to clean, deletes up to maxToDelete documents, notifying watchers
     * of ranges as they are done being deleted. It performs its own collection locking so it must
     * be called without locks.
     *
     * The 'rangeDeleterForTestOnly' is used as a utility for unit-tests that directly test the
     * CollectionRangeDeleter class so they do not need to set up
     * CollectionShardingState/MetadataManager.
     *
     * Returns true if it should be scheduled to run again because there is more data to be deleted
     * or false otherwise.
     */
    static bool cleanUpNextRange(OperationContext*,
                                 NamespaceString const& nss,
                                 int maxToDelete,
                                 CollectionRangeDeleter* rangeDeleterForTestOnly = nullptr);

private:
    /**
     * Performs the deletion of up to maxToDelete entries within the range in progress. Must be
     * called under the collection lock.
     *
     * Returns the number of documents deleted, 0 if done with the range, or bad status if deleting
     * the range failed.
     */
    StatusWith<int> _doDeletion(OperationContext* opCtx,
                                Collection* collection,
                                const BSONObj& keyPattern,
                                ChunkRange const& range,
                                int maxToDelete);

    /**
     * Removes the latest-scheduled range from the ranges to be cleaned up, and notifies any
     * interested callers of this->overlaps(range) with specified status.
     */
    void _pop(Status status);

    /**
     * Ranges scheduled for deletion.  The front of the list will be in active process of deletion.
     * As each range is completed, its notification is signaled before it is popped.
     */
    struct Deletion {
        ChunkRange const range;
        DeleteNotification const notification;
    };
    std::list<Deletion> _orphans;
};

}  // namespace mongo
