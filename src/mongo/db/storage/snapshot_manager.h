/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <limits>
#include <string>

#include "mongo/base/status.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/assert_util.h"

namespace mongo {

/**
 * Manages snapshots that can be read from at a later time.
 *
 * Implementations must be able to handle concurrent access to any methods. No methods are allowed
 * to acquire locks from the LockManager.
 */
class SnapshotManager {
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
     *  Sets the snapshot for the last stable timestamp for reading on secondaries.
     */
    virtual void setLocalSnapshot(const Timestamp& timestamp) = 0;

    /**
     * Returns the local snapshot timestamp.
     */
    virtual boost::optional<Timestamp> getLocalSnapshot() = 0;

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
