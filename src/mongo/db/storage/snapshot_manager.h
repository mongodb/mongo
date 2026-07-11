// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/timestamp.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Manages snapshots that can be read from at a later time.
 *
 * Implementations must be able to handle concurrent access to any methods. No methods are allowed
 * to acquire locks from the LockManager.
 */
class [[MONGO_MOD_OPEN]] SnapshotManager {
public:
    /**
     * Sets the snapshot to be used for committed reads.
     *
     * Implementations are allowed to assume that all older snapshots have names that compare
     * less than the passed in name, and newer ones compare greater.
     *
     * This is called while holding a very hot mutex. Therefore it should avoid doing any work that
     * can be done later.
     */
    virtual void setCommittedSnapshot(const Timestamp& timestamp) = 0;

    /**
     *  Sets the lastApplied timestamp.
     */
    virtual void setLastApplied(const Timestamp& timestamp) = 0;

    /**
     * Returns the lastApplied timestamp.
     */
    virtual boost::optional<Timestamp> getLastApplied() = 0;

    /**
     * Clears the "committed" snapshot.
     */
    virtual void clearCommittedSnapshot() = 0;

protected:
    /**
     * SnapshotManagers are not intended to be deleted through pointers to base type.
     * (virtual is just to suppress compiler warnings)
     */
    virtual ~SnapshotManager() = default;
};

}  // namespace mongo
