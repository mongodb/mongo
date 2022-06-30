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


#include "mongo/db/write_concern.h"

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/timer_stats.h"
#include "mongo/db/storage/control/journal_flusher.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/transaction_validation.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/logv2/log.h"
#include "mongo/util/fail_point.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {

using repl::OpTime;
using std::string;

static TimerStats& gleWtimeStats = makeServerStatusMetric<TimerStats>("getLastError.wtime");
static CounterMetric gleWtimeouts("getLastError.wtimeouts");
static CounterMetric gleDefaultWtimeouts("getLastError.default.wtimeouts");
static CounterMetric gleDefaultUnsatisfiable("getLastError.default.unsatisfiable");

MONGO_FAIL_POINT_DEFINE(hangBeforeWaitingForWriteConcern);

bool commandSpecifiesWriteConcern(const BSONObj& cmdObj) {
    return cmdObj.hasField(WriteConcernOptions::kWriteConcernField);
}

StatusWith<WriteConcernOptions> extractWriteConcern(OperationContext* opCtx,
                                                    const BSONObj& cmdObj,
                                                    bool isInternalClient) {
    // The default write concern if empty is {w:1}. Specifying {w:0} is/was allowed, but is
    // interpreted identically to {w:1}.
    auto wcResult = WriteConcernOptions::extractWCFromCommand(cmdObj);
    if (!wcResult.isOK()) {
        return wcResult.getStatus();
    }

    WriteConcernOptions writeConcern = wcResult.getValue();

    // This is the WC extracted from the command object, so the CWWC or implicit default hasn't been
    // applied yet, which is why "usedDefaultConstructedWC" flag can be used an indicator of whether
    // the client supplied a WC or not.
    bool clientSuppliedWriteConcern = !writeConcern.usedDefaultConstructedWC;
    bool customDefaultWasApplied = false;

    // If no write concern is specified in the command, then use the cluster-wide default WC (if
    // there is one), or else the default WC {w:1}.
    if (!clientSuppliedWriteConcern) {
        writeConcern = ([&]() {
            // WriteConcern defaults can only be applied on regular replica set members.  Operations
            // received by shard and config servers should always have WC explicitly specified.
            if (serverGlobalParams.clusterRole != ClusterRole::ShardServer &&
                serverGlobalParams.clusterRole != ClusterRole::ConfigServer &&
                repl::ReplicationCoordinator::get(opCtx)->isReplEnabled() &&
                (!opCtx->inMultiDocumentTransaction() ||
                 isTransactionCommand(cmdObj.firstElementFieldName())) &&
                !opCtx->getClient()->isInDirectClient() && !isInternalClient) {

                const auto rwcDefaults =
                    ReadWriteConcernDefaults::get(opCtx->getServiceContext()).getDefault(opCtx);
                auto wcDefault = rwcDefaults.getDefaultWriteConcern();
                if (wcDefault) {
                    const auto defaultWriteConcernSource =
                        rwcDefaults.getDefaultWriteConcernSource();
                    customDefaultWasApplied = defaultWriteConcernSource &&
                        defaultWriteConcernSource == DefaultWriteConcernSourceEnum::kGlobal;

                    LOGV2_DEBUG(22548,
                                2,
                                "Applying default writeConcern on {cmdObj_firstElementFieldName} "
                                "of {wcDefault}",
                                "cmdObj_firstElementFieldName"_attr =
                                    cmdObj.firstElementFieldName(),
                                "wcDefault"_attr = wcDefault->toBSON());
                    return *wcDefault;
                }
            }
            return writeConcern;
        })();

        if (writeConcern.isUnacknowledged()) {
            writeConcern.w = 1;
        }

        writeConcern.notExplicitWValue = true;
    }

    // It's fine for clients to provide any provenance value to mongod. But if they haven't, then an
    // appropriate provenance needs to be determined.
    auto& provenance = writeConcern.getProvenance();
    if (!provenance.hasSource()) {
        if (clientSuppliedWriteConcern) {
            provenance.setSource(ReadWriteConcernProvenance::Source::clientSupplied);
        } else if (customDefaultWasApplied) {
            provenance.setSource(ReadWriteConcernProvenance::Source::customDefault);
        } else if (opCtx->getClient()->isInDirectClient() || isInternalClient) {
            provenance.setSource(ReadWriteConcernProvenance::Source::internalWriteDefault);
        } else {
            provenance.setSource(ReadWriteConcernProvenance::Source::implicitDefault);
        }
    }

    if (!clientSuppliedWriteConcern &&
        serverGlobalParams.clusterRole == ClusterRole::ConfigServer &&
        !opCtx->getClient()->isInDirectClient() &&
        (opCtx->getClient()->session() &&
         (opCtx->getClient()->session()->getTags() & transport::Session::kInternalClient))) {
        // Upconvert the writeConcern of any incoming requests from internal connections (i.e.,
        // from other nodes in the cluster) to "majority." This protects against internal code that
        // does not specify writeConcern when writing to the config server.
        writeConcern = WriteConcernOptions{
            WriteConcernOptions::kMajority, WriteConcernOptions::SyncMode::UNSET, Seconds(30)};
        writeConcern.getProvenance().setSource(
            ReadWriteConcernProvenance::Source::internalWriteDefault);
    } else {
        Status wcStatus = validateWriteConcern(opCtx, writeConcern);
        if (!wcStatus.isOK()) {
            return wcStatus;
        }
    }

    return writeConcern;
}

Status validateWriteConcern(OperationContext* opCtx, const WriteConcernOptions& writeConcern) {
    if (writeConcern.syncMode == WriteConcernOptions::SyncMode::JOURNAL &&
        !opCtx->getServiceContext()->getStorageEngine()->isDurable()) {
        return Status(ErrorCodes::BadValue,
                      "cannot use 'j' option when a host does not have journaling enabled");
    }

    const auto replMode = repl::ReplicationCoordinator::get(opCtx)->getReplicationMode();

    if (replMode == repl::ReplicationCoordinator::modeNone &&
        (stdx::holds_alternative<int64_t>(writeConcern.w) &&
         stdx::get<int64_t>(writeConcern.w) > 1)) {
        return Status(ErrorCodes::BadValue, "cannot use 'w' > 1 when a host is not replicated");
    }

    if (replMode != repl::ReplicationCoordinator::modeReplSet &&
        writeConcern.hasCustomWriteMode()) {
        return Status(ErrorCodes::BadValue,
                      fmt::format("cannot use non-majority 'w' mode \"{}\" when a host is not a "
                                  "member of a replica set",
                                  stdx::get<std::string>(writeConcern.w)));
    }

    return Status::OK();
}

void WriteConcernResult::appendTo(BSONObjBuilder* result) const {
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

    result->append("writeConcern", wcUsed.toBSON());

    if (err.empty())
        result->appendNull("err");
    else
        result->append("err", err);
}

/**
 * Write concern with {j: true} on single voter replica set primaries must wait for no oplog holes
 * behind a write, before flushing to disk (not done in this function), in order to guarantee that
 * a write will remain after unclean shutdown and server restart recovery.
 *
 * Multi-voter replica sets will likely roll back writes if the primary crashes and restarts.
 * However, single voter sets never roll back writes, so we must maintain that behavior. Multi-node
 * single-voter primaries must truncate the oplog to ensure cross-replica set data consistency; and
 * single-node single-voter sets must never lose confirmed writes.
 *
 * The oplogTruncateAfterPoint is updated with the no holes point prior to journal flushing (write
 * persistence). Ensuring the no holes point is past (or equal to) our write, ensures the flush to
 * disk will save a truncate point that will not truncate the new write we wish to guarantee.
 *
 * Can throw on opCtx interruption.
 */
void waitForNoOplogHolesIfNeeded(OperationContext* opCtx) {
    auto const replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (replCoord->getConfigVotingMembers().size() == 1) {
        // It is safe for secondaries in multi-node single voter replica sets to truncate writes if
        // there are oplog holes. They can catch up again.
        repl::StorageInterface::get(opCtx)->waitForAllEarlierOplogWritesToBeVisible(
            opCtx, /*primaryOnly*/ true);
    }
}

Status waitForWriteConcern(OperationContext* opCtx,
                           const OpTime& replOpTime,
                           const WriteConcernOptions& writeConcern,
                           WriteConcernResult* result) {
    LOGV2_DEBUG(22549,
                2,
                "Waiting for write concern. OpTime: {replOpTime}, write concern: {writeConcern}",
                "replOpTime"_attr = replOpTime,
                "writeConcern"_attr = writeConcern.toBSON());

    auto* const storageEngine = opCtx->getServiceContext()->getStorageEngine();
    auto const replCoord = repl::ReplicationCoordinator::get(opCtx);

    if (MONGO_unlikely(hangBeforeWaitingForWriteConcern.shouldFail()) &&
        !opCtx->getClient()->isInDirectClient()) {
        // Respecting this failpoint for internal clients prevents stepup from working properly.
        // This fail point pauses with an open snapshot on the oplog. Some tests pause on this fail
        // point prior to running replication rollback. This prevents the operation from being
        // killed and the snapshot being released. Hence, we release the snapshot here.
        auto recoveryUnit = opCtx->releaseAndReplaceRecoveryUnit();
        recoveryUnit.reset();

        hangBeforeWaitingForWriteConcern.pauseWhileSet();
    }

    // Next handle blocking on disk
    Timer syncTimer;
    WriteConcernOptions writeConcernWithPopulatedSyncMode =
        replCoord->populateUnsetWriteConcernOptionsSyncMode(writeConcern);

    // Waiting for durability (flushing the journal or all files to disk) can throw on interruption.
    try {
        switch (writeConcernWithPopulatedSyncMode.syncMode) {
            case WriteConcernOptions::SyncMode::UNSET:
                LOGV2_FATAL(34410,
                            "Attempting to wait on a WriteConcern with an unset sync option");
            case WriteConcernOptions::SyncMode::NONE:
                break;
            case WriteConcernOptions::SyncMode::FSYNC: {
                waitForNoOplogHolesIfNeeded(opCtx);
                if (!storageEngine->isDurable()) {
                    storageEngine->flushAllFiles(opCtx, /*callerHoldsReadLock*/ false);

                    // This field has had a dummy value since MMAP went away. It is undocumented.
                    // Maintaining it so as not to cause unnecessary user pain across upgrades.
                    result->fsyncFiles = 1;
                } else {
                    // We only need to commit the journal if we're durable
                    JournalFlusher::get(opCtx)->waitForJournalFlush();
                }
                break;
            }
            case WriteConcernOptions::SyncMode::JOURNAL:
                waitForNoOplogHolesIfNeeded(opCtx);
                JournalFlusher::get(opCtx)->waitForJournalFlush();
                break;
        }
    } catch (const DBException& ex) {
        return ex.toStatus();
    }

    result->syncMillis = syncTimer.millis();

    // Now wait for replication

    if (replOpTime.isNull()) {
        // no write happened for this client yet
        return Status::OK();
    }

    // needed to avoid incrementing gleWtimeStats SERVER-9005
    if (!writeConcernWithPopulatedSyncMode.needToWaitForOtherNodes()) {
        // no desired replication check
        return Status::OK();
    }

    // Replica set stepdowns and gle mode changes are thrown as errors
    repl::ReplicationCoordinator::StatusAndDuration replStatus =
        replCoord->awaitReplication(opCtx, replOpTime, writeConcernWithPopulatedSyncMode);
    if (replStatus.status == ErrorCodes::WriteConcernFailed) {
        gleWtimeouts.increment();
        if (!writeConcern.getProvenance().isClientSupplied()) {
            gleDefaultWtimeouts.increment();
        }
        result->err = "timeout";
        result->wTimedOut = true;
    }
    if (replStatus.status == ErrorCodes::UnsatisfiableWriteConcern) {
        if (!writeConcern.getProvenance().isClientSupplied()) {
            gleDefaultUnsatisfiable.increment();
        }
    }

    gleWtimeStats.recordMillis(durationCount<Milliseconds>(replStatus.duration));
    result->wTime = durationCount<Milliseconds>(replStatus.duration);

    result->wcUsed = writeConcern;

    return replStatus.status;
}

}  // namespace mongo
