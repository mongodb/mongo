/**
*    Copyright (C) 2017 MongoDB Inc.
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
#include "mongo/base/status_with.h"
#include "mongo/db/repl/optime.h"

namespace mongo {

class OperationContext;

namespace repl {

class StorageInterface;
class ReplicationConsistencyMarkers;

/**
 * This class is used by the replication system to recover after an unclean shutdown or a rollback.
 */
class ReplicationRecovery {
public:
    ReplicationRecovery() = default;
    virtual ~ReplicationRecovery() = default;

    /**
     * Recovers the data on disk from the oplog.
     */
    virtual void recoverFromOplog(OperationContext* opCtx) = 0;
};

class ReplicationRecoveryImpl : public ReplicationRecovery {
    MONGO_DISALLOW_COPYING(ReplicationRecoveryImpl);

public:
    ReplicationRecoveryImpl(StorageInterface* storageInterface,
                            ReplicationConsistencyMarkers* consistencyMarkers);

    void recoverFromOplog(OperationContext* opCtx) override;

private:
    /**
     * Applies all oplog entries from oplogApplicationStartPoint (exclusive) to topOfOplog
     * (inclusive). This fasserts if oplogApplicationStartPoint is not in the oplog.
     */
    void _applyToEndOfOplog(OperationContext* opCtx,
                            const Timestamp& oplogApplicationStartPoint,
                            const Timestamp& topOfOplog);

    /**
     * Gets the last applied OpTime from the end of the oplog. Returns CollectionIsEmpty if there is
     * no oplog.
     */
    StatusWith<OpTime> _getLastAppliedOpTime(OperationContext* opCtx) const;

    /**
     * Truncates the oplog after and including the "truncateTimestamp" entry.
     */
    void _truncateOplogTo(OperationContext* opCtx, Timestamp truncateTimestamp);

    StorageInterface* _storageInterface;
    ReplicationConsistencyMarkers* _consistencyMarkers;
};

}  // namespace repl
}  // namespace mongo
