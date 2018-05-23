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

#include <list>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/namespace_string.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/time_support.h"

namespace mongo {

class BSONObj;
class Collection;
class OperationContext;

// After completing a batch of document deletions, the time in millis to wait before commencing the
// next batch of deletions.
extern AtomicInt32 rangeDeleterBatchDelayMS;

class CollectionRangeDeleter {
    MONGO_DISALLOW_COPYING(CollectionRangeDeleter);

public:
    /**
      * This is an object n that asynchronously changes state when a scheduled range deletion
      * completes or fails. Call n.ready() to discover if the event has already occurred.  Call
      * n.waitStatus(opCtx) to sleep waiting for the event, and get its result.  If the wait is
      * interrupted, waitStatus throws.
      *
      * It is an error to destroy a returned CleanupNotification object n unless either n.ready()
      * is true or n.abandon() has been called.  After n.abandon(), n is in a moved-from state.
      */
    class DeleteNotification {
    public:
        DeleteNotification();
        DeleteNotification(Status status);

        // The following default declarations are needed because the presence of a non-trivial
        // destructor forbids the compiler to generate the declarations itself, but the definitions
        // it generates are fine.
        DeleteNotification(DeleteNotification&& notifn) = default;
        DeleteNotification& operator=(DeleteNotification&& notifn) = default;
        DeleteNotification(DeleteNotification const& notifn) = default;
        DeleteNotification& operator=(DeleteNotification const& notifn) = default;

        ~DeleteNotification() {
            // Can be null only if moved from
            dassert(!_notification || *_notification || _notification.use_count() == 1);
        }

        void notify(Status status) const {
            _notification->set(status);
        }

        /**
         * Sleeps waiting for notification, and returns notify's argument. On interruption, throws;
         * calling waitStatus afterward returns failed status.
         */
        Status waitStatus(OperationContext* opCtx);

        bool ready() const {
            return bool(*_notification);
        }
        void abandon() {
            _notification = nullptr;
        }
        bool operator==(DeleteNotification const& other) const {
            return _notification == other._notification;
        }
        bool operator!=(DeleteNotification const& other) const {
            return _notification != other._notification;
        }

    private:
        std::shared_ptr<Notification<Status>> _notification;
    };

    struct Deletion {
        Deletion(ChunkRange r, Date_t when) : range(std::move(r)), whenToDelete(when) {}

        ChunkRange range;
        Date_t whenToDelete;  // A value of Date_t{} means immediately.
        DeleteNotification notification{};
    };

    CollectionRangeDeleter();
    ~CollectionRangeDeleter();

    //
    // All of the following members must be called only while the containing MetadataManager's lock
    // is held (or in its destructor), except cleanUpNextRange.
    //

    /**
     * Splices range's elements to the list to be cleaned up by the deleter thread.  Deletions d
     * with d.delay == true are added to the delayed queue, and scheduled at time `later`.
     * Returns the time to begin deletions, if needed, or boost::none if deletions are already
     * scheduled.
     */
    boost::optional<Date_t> add(std::list<Deletion> ranges);

    /**
     * Reports whether the argument range overlaps any of the ranges to clean.  If there is overlap,
     * it returns a notification that will be signaled when the currently newest overlapping range
     * completes or fails. If there is no overlap, the result is boost::none.  After a successful
     * removal, the caller should call again to ensure no other range overlaps the argument.
     * (See CollectionShardingState::waitForClean and MetadataManager::trackOrphanedDataCleanup for
     * an example use.)
     */
    boost::optional<DeleteNotification> overlaps(ChunkRange const& range) const;

    /**
     * Reports the number of ranges remaining to be cleaned up.
     */
    size_t size() const;

    bool isEmpty() const;

    /*
     * Notifies with the specified status anything waiting on ranges scheduled, and then discards
     * the ranges and notifications.  Is called in the destructor.
     */
    void clear(Status status);

    /*
     * Append a representation of self to the specified builder.
     */
    void append(BSONObjBuilder* builder) const;

    /**
     * If any range deletions are scheduled, deletes up to maxToDelete documents, notifying
     * watchers of ranges as they are done being deleted. It performs its own collection locking, so
     * it must be called without locks.
     *
     * If it should be scheduled to run again because there might be more documents to delete,
     * returns the time to begin, or boost::none otherwise.
     *
     * Argument 'forTestOnly' is used in unit tests that exercise the CollectionRangeDeleter class,
     * so that they do not need to set up CollectionShardingState and MetadataManager objects.
     */
    static boost::optional<Date_t> cleanUpNextRange(OperationContext*,
                                                    NamespaceString const& nss,
                                                    OID const& epoch,
                                                    int maxToDelete,
                                                    CollectionRangeDeleter* forTestOnly = nullptr);

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
    std::list<Deletion> _orphans;
    std::list<Deletion> _delayedOrphans;
};

}  // namespace mongo
