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

#include "mongo/base/status_with.h"
#include "mongo/db/repl/optime.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {

class OperationContext;
class Status;

namespace repl {

class ReplicationCoordinator;

/**
 * SyncSourceResolverResponse contains the result of a call to findSyncSource. This result will
 * indicate one of the following:
 *          1. A new sync source was selected. isOK() will return true and getSyncSource() will
 *              return the HostAndPort of the new sync source.
 *          2. No sync source was selected. isOK() will return true and getSyncSource() will return
 *              an empty HostAndPort.
 *          3. All potential sync sources are too fresh. isOK() will return false and
 *              syncSourceStatus will be ErrorCodes::OplogStartMissing and earliestOpTimeSeen will
 *              contain a new MinValid boundry. getSyncSource() is not valid to call in this state.
 */
struct SyncSourceResolverResponse {
    // Contains the new syncSource if syncSourceStatus is OK and the HostAndPort is not empty.
    StatusWith<HostAndPort> syncSourceStatus = {ErrorCodes::BadValue, "status not populated"};

    // Contains the new MinValid boundry if syncSourceStatus is ErrorCodes::OplogStartMissing.
    OpTime earliestOpTimeSeen;

    bool isOK() {
        return syncSourceStatus.isOK();
    }

    HostAndPort getSyncSource() {
        invariant(syncSourceStatus.isOK());
        return syncSourceStatus.getValue();
    }
};

/**
 * Supplies a sync source to Fetcher, Rollback and Reporter.
 */
class SyncSourceResolver {
public:
    SyncSourceResolver(ReplicationCoordinator* replCoord) : _replCoord(replCoord){};

    /**
     * Uses the provided lastOpTimeFetched and replCoord to find a new sync source for
     * DataReplicator components.
     */
    SyncSourceResolverResponse findSyncSource(OperationContext* txn,
                                              const OpTime& lastOpTimeFetched);

    /**
     * Returns current sync source, which may be empty if there is no valid sync source available.
     */
    HostAndPort getActiveSyncSource();

private:
    ReplicationCoordinator* _replCoord;
    // Protects _syncSource.
    stdx::mutex _mutex;
    HostAndPort _syncSource;
};

}  // namespace repl
}  // namespace mongo
