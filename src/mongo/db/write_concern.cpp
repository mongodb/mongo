/**
 *    Copyright (C) 2013 10gen Inc.
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

#include "mongo/db/write_concern.h"

#include "mongo/base/counter.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/timer_stats.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/rpc/protocol.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"

namespace mongo {

using std::string;
using repl::OpTime;

static TimerStats gleWtimeStats;
static ServerStatusMetricField<TimerStats> displayGleLatency("getLastError.wtime", &gleWtimeStats);

static Counter64 gleWtimeouts;
static ServerStatusMetricField<Counter64> gleWtimeoutsDisplay("getLastError.wtimeouts",
                                                              &gleWtimeouts);

MONGO_FP_DECLARE(hangBeforeWaitingForWriteConcern);

bool commandSpecifiesWriteConcern(const BSONObj& cmdObj) {
    return cmdObj.hasField(WriteConcernOptions::kWriteConcernField);
}

StatusWith<WriteConcernOptions> extractWriteConcern(OperationContext* opCtx,
                                                    const BSONObj& cmdObj,
                                                    const std::string& dbName) {
    // The default write concern if empty is {w:1}. Specifying {w:0} is/was allowed, but is
    // interpreted identically to {w:1}.
    auto wcResult = WriteConcernOptions::extractWCFromCommand(
        cmdObj, dbName, repl::ReplicationCoordinator::get(opCtx)->getGetLastErrorDefault());
    if (!wcResult.isOK()) {
        return wcResult.getStatus();
    }

    WriteConcernOptions writeConcern = wcResult.getValue();

    if (writeConcern.usedDefault) {
        if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer &&
            !opCtx->getClient()->isInDirectClient()) {
            // This is here only for backwards compatibility with 3.2 clusters which have commands
            // that do not specify write concern when writing to the config server.
            writeConcern = {
                WriteConcernOptions::kMajority, WriteConcernOptions::SyncMode::UNSET, Seconds(30)};
        }
    } else {
        Status wcStatus = validateWriteConcern(opCtx, writeConcern, dbName);
        if (!wcStatus.isOK()) {
            return wcStatus;
        }
    }

    return writeConcern;
}

Status validateWriteConcern(OperationContext* opCtx,
                            const WriteConcernOptions& writeConcern,
                            StringData dbName) {
    if (writeConcern.syncMode == WriteConcernOptions::SyncMode::JOURNAL &&
        !opCtx->getServiceContext()->getGlobalStorageEngine()->isDurable()) {
        return Status(ErrorCodes::BadValue,
                      "cannot use 'j' option when a host does not have journaling enabled");
    }

    const auto replMode = repl::ReplicationCoordinator::get(opCtx)->getReplicationMode();

    if (replMode == repl::ReplicationCoordinator::modeNone && writeConcern.wNumNodes > 1) {
        return Status(ErrorCodes::BadValue, "cannot use 'w' > 1 when a host is not replicated");
    }

    if (replMode != repl::ReplicationCoordinator::modeReplSet && !writeConcern.wMode.empty() &&
        writeConcern.wMode != WriteConcernOptions::kMajority) {
        return Status(ErrorCodes::BadValue,
                      string("cannot use non-majority 'w' mode ") + writeConcern.wMode +
                          " when a host is not a member of a replica set");
    }

    return Status::OK();
}

void WriteConcernResult::appendTo(const WriteConcernOptions& writeConcern,
                                  BSONObjBuilder* result) const {
    if (syncMillis >= 0)
        result->appendNumber("syncMillis", syncMillis);

    if (fsyncFiles >= 0)
        result->appendNumber("fsyncFiles", fsyncFiles);

    if (wTime >= 0) {
        if (wTimedOut)
            result->appendNumber("waited", wTime);
        else
            result->appendNumber("wtime", wTime);
    }

    if (wTimedOut)
        result->appendBool("wtimeout", true);

    if (writtenTo.size()) {
        BSONArrayBuilder hosts(result->subarrayStart("writtenTo"));
        for (size_t i = 0; i < writtenTo.size(); ++i) {
            hosts.append(writtenTo[i].toString());
        }
    } else {
        result->appendNull("writtenTo");
    }

    if (err.empty())
        result->appendNull("err");
    else
        result->append("err", err);

    // For ephemeral storage engines, 0 files may be fsynced
    invariant(writeConcern.syncMode != WriteConcernOptions::SyncMode::FSYNC ||
              (result->asTempObj()["fsyncFiles"].numberLong() >= 0 ||
               !result->asTempObj()["waited"].eoo()));
}

Status waitForWriteConcern(OperationContext* opCtx,
                           const OpTime& replOpTime,
                           const WriteConcernOptions& writeConcern,
                           WriteConcernResult* result) {
    LOG(2) << "Waiting for write concern. OpTime: " << replOpTime
           << ", write concern: " << writeConcern.toBSON();
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);

    MONGO_FAIL_POINT_PAUSE_WHILE_SET(hangBeforeWaitingForWriteConcern);

    // Next handle blocking on disk
    Timer syncTimer;
    WriteConcernOptions writeConcernWithPopulatedSyncMode =
        replCoord->populateUnsetWriteConcernOptionsSyncMode(writeConcern);

    switch (writeConcernWithPopulatedSyncMode.syncMode) {
        case WriteConcernOptions::SyncMode::UNSET:
            severe() << "Attempting to wait on a WriteConcern with an unset sync option";
            fassertFailed(34410);
        case WriteConcernOptions::SyncMode::NONE:
            break;
        case WriteConcernOptions::SyncMode::FSYNC: {
            StorageEngine* storageEngine = getGlobalServiceContext()->getGlobalStorageEngine();
            if (!storageEngine->isDurable()) {
                result->fsyncFiles = storageEngine->flushAllFiles(opCtx, true);
            } else {
                // We only need to commit the journal if we're durable
                opCtx->recoveryUnit()->waitUntilDurable();
            }
            break;
        }
        case WriteConcernOptions::SyncMode::JOURNAL:
            if (replCoord->getReplicationMode() != repl::ReplicationCoordinator::Mode::modeNone) {
                // Wait for ops to become durable then update replication system's
                // knowledge of this.
                OpTime appliedOpTime = replCoord->getMyLastAppliedOpTime();
                opCtx->recoveryUnit()->waitUntilDurable();
                replCoord->setMyLastDurableOpTimeForward(appliedOpTime);
            } else {
                opCtx->recoveryUnit()->waitUntilDurable();
            }
            break;
    }

    result->syncMillis = syncTimer.millis();

    // Now wait for replication

    if (replOpTime.isNull()) {
        // no write happened for this client yet
        return Status::OK();
    }

    // needed to avoid incrementing gleWtimeStats SERVER-9005
    if (writeConcernWithPopulatedSyncMode.wNumNodes <= 1 &&
        writeConcernWithPopulatedSyncMode.wMode.empty()) {
        // no desired replication check
        return Status::OK();
    }

    // Replica set stepdowns and gle mode changes are thrown as errors
    repl::ReplicationCoordinator::StatusAndDuration replStatus =
        replCoord->awaitReplication(opCtx, replOpTime, writeConcernWithPopulatedSyncMode);
    if (replStatus.status == ErrorCodes::WriteConcernFailed) {
        gleWtimeouts.increment();
        result->err = "timeout";
        result->wTimedOut = true;
    }

    // Add stats
    result->writtenTo = repl::getGlobalReplicationCoordinator()->getHostsWrittenTo(
        replOpTime,
        writeConcernWithPopulatedSyncMode.syncMode == WriteConcernOptions::SyncMode::JOURNAL);
    gleWtimeStats.recordMillis(durationCount<Milliseconds>(replStatus.duration));
    result->wTime = durationCount<Milliseconds>(replStatus.duration);

    return replStatus.status;
}

}  // namespace mongo
