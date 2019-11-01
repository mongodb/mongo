/**
 *    Copyright (C) 2019-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/concurrency/mutex.h"

namespace mongo {
namespace biggie {

class RecordStore;

/**
 * Manages oplog visibility by keeping track of uncommitted RecordIds and hiding Records from
 * cursors while a given Record's RecordId is greater than the uncommitted RecordIds.
 */
class VisibilityManager {
public:
    /**
     * Removes the RecordId from the uncommitted records and notifies other threads that a chunk of
     * the oplog became visible.
     */
    void dealtWithRecord(RecordId rid);

    /**
     * Adds a RecordId to be tracked while its Record is uncommitted. Upon commit or rollback of
     * the record, the appropriate actions are taken to change the visibility of the oplog.
     */
    void addUncommittedRecord(OperationContext* opCtx, RecordStore* rs, RecordId rid);

    /**
     * Returns the highest seen RecordId such that it and all smaller RecordIds are committed or
     * rolled back.
     */
    RecordId getAllCommittedRecord();

    /**
     * Returns true if the given RecordId is the earliest uncommitted Record being tracked by the
     * visibility manager, otherwise it returns false.
     */
    bool isFirstHidden(RecordId rid);

    /**
     * Uses a condition variable to have all threads wait until all earlier oplog writes are visible
     * based on the RecordId they're waiting for to become visible.
     */
    void waitForAllEarlierOplogWritesToBeVisible(OperationContext* opCtx);

private:
    mutable Mutex _stateLock =
        MONGO_MAKE_LATCH("VisibilityManager::_stateLock");  // Protects the values below.
    RecordId _highestSeen = RecordId();

    // Used to wait for all earlier oplog writes to be visible.
    mutable stdx::condition_variable _opsBecameVisibleCV;
    std::set<RecordId> _uncommittedRecords;  // RecordIds that have yet to be committed/rolled back.
};

}  // namespace biggie
}  // namespace mongo
