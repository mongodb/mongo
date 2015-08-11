/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include <limits>
#include <string>

#include "mongo/base/status.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/snapshot_name.h"
#include "mongo/util/assert_util.h"

namespace mongo {

class RecoveryUnit;

/**
 * Manages snapshots that can be read from at a later time.
 *
 * Implementations must be able to handle concurrent access to any methods. No methods are allowed
 * to acquire locks from the LockManager.
 */
class SnapshotManager {
public:
    /**
     * Prepares the passed-in OperationContext for snapshot creation.
     *
     * The passed-in OperationContext will be associated with a point-in-time that can be used
     * for creating a snapshot later.
     *
     * This must be the first method called after starting a ScopedTransaction, and it is
     * illegal to start a WriteUnitOfWork inside of the same ScopedTransaction.
     */
    virtual Status prepareForCreateSnapshot(OperationContext* txn) = 0;

    /**
     * Creates a new named snapshot representing the same point-in-time captured in
     * prepareForCreateSnapshot().
     *
     * Must be called in the same ScopedTransaction as prepareForCreateSnapshot.
     *
     * Caller guarantees that this name must compare greater than all existing snapshots.
     */
    virtual Status createSnapshot(OperationContext* txn, const SnapshotName& name) = 0;

    /**
     * Sets the snapshot to be used for committed reads.
     *
     * Implementations are allowed to assume that all older snapshots have names that compare
     * less than the passed in name, and newer ones compare greater.
     *
     * This is called while holding a very hot mutex. Therefore it should avoid doing any work that
     * can be done later. In particular, cleaning up of old snapshots should be deferred until
     * cleanupUnneededSnapshots is called.
     */
    virtual void setCommittedSnapshot(const SnapshotName& name) = 0;

    /**
     * Cleans up all snapshots older than the current committed snapshot.
     *
     * Operations that have already begun using an older snapshot must continue to work using that
     * snapshot until they would normally start using a newer one. Any implementation that allows
     * that without an unbounded growth of snapshots is permitted.
     */
    virtual void cleanupUnneededSnapshots() = 0;

    /**
     * Drops all snapshots and clears the "committed" snapshot.
     */
    virtual void dropAllSnapshots() = 0;

protected:
    /**
     * SnapshotManagers are not intended to be deleted through pointers to base type.
     * (virtual is just to suppress compiler warnings)
     */
    virtual ~SnapshotManager() = default;
};

}  // namespace mongo
