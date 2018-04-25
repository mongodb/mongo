/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
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

#include <boost/optional.hpp>
#include <wiredtiger.h>

#include "mongo/base/disallow_copying.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/storage/snapshot_manager.h"
#include "mongo/stdx/mutex.h"

namespace mongo {

class WiredTigerOplogManager;

class WiredTigerSnapshotManager final : public SnapshotManager {
    MONGO_DISALLOW_COPYING(WiredTigerSnapshotManager);

public:
    WiredTigerSnapshotManager() = default;

    void setCommittedSnapshot(const Timestamp& timestamp) final;
    void setLocalSnapshot(const Timestamp& timestamp) final;
    boost::optional<Timestamp> getLocalSnapshot() final;
    void dropAllSnapshots() final;

    //
    // WT-specific methods
    //

    /**
     * Starts a transaction and returns the SnapshotName used.
     *
     * Throws if there is currently no committed snapshot.
     */
    Timestamp beginTransactionOnCommittedSnapshot(WT_SESSION* session) const;

    /**
     * Starts a transaction on the last stable local timestamp, set by setLocalSnapshot.
     *
     * Throws if no local snapshot has been set.
     */
    Timestamp beginTransactionOnLocalSnapshot(WT_SESSION* session, bool ignorePrepare) const;

    /**
     * Returns lowest SnapshotName that could possibly be used by a future call to
     * beginTransactionOnCommittedSnapshot, or boost::none if there is currently no committed
     * snapshot.
     *
     * This should not be used for starting a transaction on this SnapshotName since the named
     * snapshot may be deleted by the time you start the transaction.
     */
    boost::optional<Timestamp> getMinSnapshotForNextCommittedRead() const;

private:
    // Snapshot to use for reads at a commit timestamp.
    mutable stdx::mutex _committedSnapshotMutex;  // Guards _committedSnapshot.
    boost::optional<Timestamp> _committedSnapshot;

    // Snapshot to use for reads at a local stable timestamp.
    mutable stdx::mutex _localSnapshotMutex;  // Guards _localSnapshot.
    boost::optional<Timestamp> _localSnapshot;
};
}
