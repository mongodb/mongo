/**
*    Copyright (C) 2008 10gen Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/initial_sync.h"

#include "mongo/db/client.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/util/exit.h"
#include "mongo/util/log.h"

namespace mongo {
namespace repl {

unsigned replSetForceInitialSyncFailure = 0;

InitialSync::InitialSync(BackgroundSync* q, MultiSyncApplyFunc func) : SyncTail(q, func) {}

InitialSync::~InitialSync() {}

/* initial oplog application, during initial sync, after cloning.
*/
void InitialSync::oplogApplication(OperationContext* txn, const OpTime& endOpTime) {
    if (replSetForceInitialSyncFailure > 0) {
        log() << "test code invoked, forced InitialSync failure: "
              << replSetForceInitialSyncFailure;
        replSetForceInitialSyncFailure--;
        throw DBException("forced error", 0);
    }
    _applyOplogUntil(txn, endOpTime);
}


/* applies oplog from "now" until endOpTime using the applier threads for initial sync*/
void InitialSync::_applyOplogUntil(OperationContext* txn, const OpTime& endOpTime) {
    unsigned long long bytesApplied = 0;
    unsigned long long entriesApplied = 0;
    while (true) {
        OpQueue ops;

        auto replCoord = repl::ReplicationCoordinator::get(txn);
        while (!tryPopAndWaitForMore(txn, &ops)) {
            if (inShutdown()) {
                return;
            }

            // nothing came back last time, so go again
            if (ops.empty())
                continue;

            // Check if we reached the end
            const BSONObj currentOp = ops.back().raw;
            const OpTime currentOpTime =
                fassertStatusOK(28772, OpTime::parseFromOplogEntry(currentOp));

            // When we reach the end return this batch
            if (currentOpTime == endOpTime) {
                break;
            } else if (currentOpTime > endOpTime) {
                severe() << "Applied past expected end " << endOpTime << " to " << currentOpTime
                         << " without seeing it. Rollback?";
                fassertFailedNoTrace(18693);
            }

            // apply replication batch limits
            if (ops.getBytes() > replBatchLimitBytes)
                break;
            if (ops.getCount() > replBatchLimitOperations)
                break;
        };

        if (ops.empty()) {
            severe() << "got no ops for batch...";
            fassertFailedNoTrace(18692);
        }

        const BSONObj lastOp = ops.back().raw.getOwned();

        // Tally operation information and apply batch. Don't use ops again after these lines.
        bytesApplied += ops.getBytes();
        entriesApplied += ops.getCount();
        const OpTime lastOpTime = multiApply(txn, ops.releaseBatch());

        replCoord->setMyLastAppliedOpTime(lastOpTime);
        setNewTimestamp(lastOpTime.getTimestamp());

        if (inShutdown()) {
            return;
        }

        // if the last op applied was our end, return
        if (lastOpTime == endOpTime) {
            LOG(1) << "SyncTail applied " << entriesApplied << " entries (" << bytesApplied
                   << " bytes) and finished at opTime " << endOpTime;
            return;
        }
    }  // end of while (true)
}
}  // namespace repl
}  // namespace mongo
