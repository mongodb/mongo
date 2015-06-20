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

#include "mongo/base/string_data.h"
#include "mongo/bson/oid.h"
#include "mongo/util/time_support.h"

namespace mongo {

class LockpingsType;
class LocksType;
class Status;
template <typename T>
class StatusWith;

/**
 * Interface for the distributed lock operations.
 */
class DistLockCatalog {
public:
    /**
     * Simple data structure for storing server local time and election id.
     */
    struct ServerInfo {
    public:
        ServerInfo();  // TODO: SERVER-18007
        ServerInfo(Date_t time, OID electionId);

        // The local time of the server at the time this was created.
        Date_t serverTime;

        // The election id of the replica set member at the time this was created.
        OID electionId;
    };

    virtual ~DistLockCatalog() = default;

    /**
     * Returns the ping document of the specified processID.
     * Common status errors include socket errors.
     */
    virtual StatusWith<LockpingsType> getPing(StringData processID) = 0;

    /**
     * Updates the ping document. Creates a new entry if it does not exists.
     * Common status errors include socket errors.
     */
    virtual Status ping(StringData processID, Date_t ping) = 0;

    /**
     * Attempts to update the owner of a lock identified by lockID to lockSessionID.
     * Will only be successful if lock is not held.
     *
     * The other parameters are for diagnostic purposes:
     * - who: unique string for the caller trying to grab the lock.
     * - processId: unique string for the process trying to grab the lock.
     * - time: the time when this is attempted.
     * - why: reason for taking the lock.
     *
     * Returns the result of the operation.
     * Returns LockStateChangeFailed if the lock acquisition cannot be done because lock
     * is already held elsewhere.
     *
     * Common status errors include socket and duplicate key errors.
     */
    virtual StatusWith<LocksType> grabLock(StringData lockID,
                                           const OID& lockSessionID,
                                           StringData who,
                                           StringData processId,
                                           Date_t time,
                                           StringData why) = 0;

    /**
     * Attempts to forcefully transfer the ownership of a lock from currentHolderTS
     * to lockSessionID.
     *
     * The other parameters are for diagnostic purposes:
     * - who: unique string for the caller trying to grab the lock.
     * - processId: unique string for the process trying to grab the lock.
     * - time: the time when this is attempted.
     * - why: reason for taking the lock.
     *
     * Returns the result of the operation.
     * Returns LockStateChangeFailed if the lock acquisition fails.
     *
     * Common status errors include socket errors.
     */
    virtual StatusWith<LocksType> overtakeLock(StringData lockID,
                                               const OID& lockSessionID,
                                               const OID& currentHolderTS,
                                               StringData who,
                                               StringData processId,
                                               Date_t time,
                                               StringData why) = 0;

    /**
     * Attempts to set the state of the lock document with lockSessionID to unlocked.
     * Common status errors include socket errors.
     */
    virtual Status unlock(const OID& lockSessionID) = 0;

    /**
     * Get some information from the config server primary.
     * Common status errors include socket errors.
     */
    virtual StatusWith<ServerInfo> getServerInfo() = 0;

    /**
     * Returns the lock document.
     * Returns LockNotFound if lock document doesn't exist.
     * Common status errors include socket errors.
     */
    virtual StatusWith<LocksType> getLockByTS(const OID& lockSessionID) = 0;

    /**
     * Returns the lock document.
     * Common status errors include socket errors.
     */
    virtual StatusWith<LocksType> getLockByName(StringData name) = 0;

    /**
     * Attempts to delete the ping document corresponding to the given processId.
     * Common status errors include socket errors.
     */
    virtual Status stopPing(StringData processId) = 0;
};
}
