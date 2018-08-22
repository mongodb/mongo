/**
 *    Copyright (C) 2018 MongoDB Inc.
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

#include "mongo/db/concurrency/lock_manager.h"

namespace mongo {

/**
 * This friend class to the LockManager extends LockManager functionality to enable behaviors
 * required for replication state transitions, which must atomically release the global X lock and
 * restore locks for prepared transactions into their individual Lockers.
 */
class ReplicationLockManagerManipulator {
    MONGO_DISALLOW_COPYING(ReplicationLockManagerManipulator);

public:
    explicit ReplicationLockManagerManipulator(LockManager* lockManager);
    ~ReplicationLockManagerManipulator() = default;

    /**
     * Works like LockManager::lock() except that it only works for the Global lock and rather than
     * looking up the true LockHead for the global resource ID, it puts the LockRequest into the
     * given TemporaryResourceQueue, which is guaranteed not to have any conflicting locks for the
     * given request.
     */
    void lockUncontestedTemporaryGlobalResource(
        LockManager::TemporaryResourceQueue* tempGlobalResource,
        LockRequest* request,
        LockMode mode);

    /**
     * Takes the locks from a given TemporaryResourceQueue for the Global resource and moves them
     * into the true LockHead for the global resource, atomically releasing the global X lock from
     * the true LockHead for the global resource in the process.
     */
    void replaceGlobalLocksWithLocksFromTemporaryGlobalResource(
        ResourceId resId, LockManager::TemporaryResourceQueue* tempGlobalResource);


private:
    LockManager* _lockManager;
};

}  // namespace mongo
