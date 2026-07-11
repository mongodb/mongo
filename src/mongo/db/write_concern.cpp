// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/write_concern.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/server_status/server_status_metric.h"
#include "mongo/db/curop.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/read_write_concern_defaults_gen.h"
#include "mongo/db/read_write_concern_provenance.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/timer_stats.h"
#include "mongo/db/storage/control/journal_flusher.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/transaction_validation.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/logv2/log.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_service.h"
#include "mongo/otel/metrics/server_status_options.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/timer.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <variant>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {

std::function<void(WriteConcernOptions&)> remapWriteConcernHook;

using repl::OpTime;
using std::string;

namespace {

using otel::metrics::MetricNames;
using otel::metrics::MetricsService;
using otel::metrics::MetricUnit;
using otel::metrics::ServerStatusOptions;

auto& gleWtimeNumMetric = MetricsService::instance().createInt64Counter(
    MetricNames::kGetLastErrorWtimeNum,
    "The total number of operations with a specified write concern (i.e. `w`) that wait for one or "
    "more members of a replica set to acknowledge the write operation (i.e. `w > 1`).",
    MetricUnit::kOperations,
    {.serverStatusOptions = ServerStatusOptions({.dottedPath = "getLastError.wtime.num"})});

auto& gleWTimeTotalMillisMetric = MetricsService::instance().createInt64Counter(
    MetricNames::kGetLastErrorWtimeTotalMillis,
    "The total amount of time in milliseconds that the mongod has spent performing operations with "
    "a write concern (i.e. `w`) that waits for one or more members of a replica set to acknowledge "
    "the write operation (i.e. a `w` value greater than `1`.).",
    MetricUnit::kMilliseconds,
    {.serverStatusOptions = ServerStatusOptions({.dottedPath = "getLastError.wtime.totalMillis"})});

auto& gleWtimeoutsMetric = MetricsService::instance().createInt64Counter(
    MetricNames::kGetLastErrorWtimeouts,
    "The number of times that write concern operations have timed out as a result of the "
    "`wtimeout` threshold. This number increments for both default and non-default write concern "
    "specifications.",
    MetricUnit::kOperations,
    {.serverStatusOptions = ServerStatusOptions({.dottedPath = "getLastError.wtimeouts"})});

auto& gleDefaultWtimeoutsMetric = MetricsService::instance().createInt64Counter(
    MetricNames::kGetLastErrorDefaultWtimeouts,
    "The number of times a non-`clientSupplied` write concern timed out.",
    MetricUnit::kOperations,
    {.serverStatusOptions = ServerStatusOptions({.dottedPath = "getLastError.default.wtimeouts"})});

auto& gleDefaultUnsatisfiableMetric = MetricsService::instance().createInt64Counter(
    MetricNames::kGetLastErrorDefaultUnsatisfiable,
    "The number of times that a non-`clientSupplied` write concern returned the "
    "`UnsatisfiableWriteConcern` error code.",
    MetricUnit::kOperations,
    {.serverStatusOptions =
         ServerStatusOptions({.dottedPath = "getLastError.default.unsatisfiable"})});

}  // namespace


MONGO_FAIL_POINT_DEFINE(hangBeforeWaitingForWriteConcern);
MONGO_FAIL_POINT_DEFINE(failWaitForWriteConcernIfTimeoutSet);

bool commandSpecifiesWriteConcern(const GenericArguments& requestArgs) {
    return !!requestArgs.getWriteConcern();
}

boost::optional<repl::ReplicationCoordinator::StatusAndDuration>
_tryGetWCFailureFromFailPoint_ForTest(const OpTime& replOpTime,
                                      const WriteConcernOptions& writeConcern) {
    bool shouldMockError = false;
    failWaitForWriteConcernIfTimeoutSet.executeIf(
        [&](const BSONObj& data) { shouldMockError = true; },
        [&](const BSONObj& data) {
            return writeConcern.wTimeout != WriteConcernOptions::kNoTimeout;
        });

    if (shouldMockError) {
        LOGV2(10431701,
              "Failing write concern wait due to failpoint",
              "threadName"_attr = getThreadName(),
              "replOpTime"_attr = replOpTime,
              "writeConcern.isMajority()"_attr = writeConcern.isMajority(),
              "writeConcern"_attr = writeConcern.toBSON());
        auto mockStatus = Status(ErrorCodes::WriteConcernTimeout, "Mock write concern timeout");
        return repl::ReplicationCoordinator::StatusAndDuration(mockStatus, Milliseconds(0));
    } else {
        return boost::none;
    }
}


StatusWith<WriteConcernOptions> extractWriteConcern(OperationContext* opCtx,
                                                    const GenericArguments& genericArgs,
                                                    std::string_view commandName,
                                                    bool isInternalClient) {
    WriteConcernOptions writeConcern =
        genericArgs.getWriteConcern().value_or_eval([]() { return WriteConcernOptions(); });

    // This is the WC extracted from the command object, so the CWWC or implicit default hasn't been
    // applied yet, which is why "usedDefaultConstructedWC" flag can be used an indicator of whether
    // the client supplied a WC or not.
    // If the user supplied write concern from the command is empty (writeConcern: {}),
    // usedDefaultConstructedWC will be true so we will then use the CWWC or implicit default.
    // Note that specifying writeConcern: {w:0} is not the same as empty. {w:0} differs from {w:1}
    // in that the client will not expect a command reply/acknowledgement at all, even in the case
    // of errors.
    bool clientSuppliedWriteConcern = !writeConcern.usedDefaultConstructedWC;
    bool customDefaultWasApplied = false;

    // Though the mongoS should always supply a write concern for shardsvr/configsvr nodes, we still
    // apply the default here for direct shard operations (or for normal replica set members).
    bool canApplyDefaultWC = repl::ReplicationCoordinator::get(opCtx)->getSettings().isReplSet() &&
        (!opCtx->inMultiDocumentTransaction() ||
         isTransactionCommand(opCtx->getService(), commandName)) &&
        !opCtx->getClient()->isInDirectClient() && !isInternalClient;


    // If no write concern is specified in the command, then use the cluster-wide default WC (if
    // there is one), or else the default implicit WC:
    // (if [(#arbiters > 0) AND (#arbiters >= ½(#voting nodes) - 1)] then {w:1} else {w:majority}).
    if (canApplyDefaultWC) {
        auto getDefaultWC = ([&]() {
            auto rwcDefaults = ReadWriteConcernDefaults::get(opCtx).getDefault(opCtx);
            auto wcDefault = rwcDefaults.getDefaultWriteConcern();
            const auto defaultWriteConcernSource = rwcDefaults.getDefaultWriteConcernSource();
            customDefaultWasApplied = defaultWriteConcernSource &&
                defaultWriteConcernSource == DefaultWriteConcernSourceEnum::kGlobal;
            return wcDefault;
        });


        if (!clientSuppliedWriteConcern) {
            writeConcern = ([&]() {
                auto wcDefault = getDefaultWC();
                // Default WC can be 'boost::none' if the implicit default is used and set to 'w:1'.
                if (wcDefault) {
                    LOGV2_DEBUG(22548,
                                2,
                                "Applying default writeConcern",
                                "commandName"_attr = commandName,
                                "wcDefault"_attr = wcDefault->toBSON());
                    return *wcDefault;
                }
                return writeConcern;
            })();
            writeConcern.notExplicitWValue = true;
        }
        // Client supplied a write concern object without 'w' field.
        else if (writeConcern.isExplicitWithoutWField()) {
            auto wcDefault = getDefaultWC();
            // Default WC can be 'boost::none' if the implicit default is used and set to 'w:1'.
            if (wcDefault) {
                clientSuppliedWriteConcern = false;
                writeConcern.w = wcDefault->w;
                if (writeConcern.syncMode == WriteConcernOptions::SyncMode::UNSET) {
                    writeConcern.syncMode = wcDefault->syncMode;
                }
            }
        }
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

    if (writeConcern.syncMode == WriteConcernOptions::SyncMode::NONE && writeConcern.isMajority() &&
        !opCtx->getServiceContext()->getStorageEngine()->isEphemeral()) {
        auto* const replCoord = repl::ReplicationCoordinator::get(opCtx);
        if (replCoord && replCoord->getSettings().isReplSet() &&
            replCoord->getWriteConcernMajorityShouldJournal()) {
            LOGV2_DEBUG(8668500,
                        1,
                        "Overriding write concern majority j:false to j:true",
                        "writeConcern"_attr = writeConcern);
            writeConcern.majorityJFalseOverridden = true;
            writeConcern.syncMode = WriteConcernOptions::SyncMode::JOURNAL;
        }
    }

    if (remapWriteConcernHook) {
        remapWriteConcernHook(writeConcern);
    }

    Status wcStatus = validateWriteConcern(opCtx, writeConcern);
    if (!wcStatus.isOK()) {
        return wcStatus;
    }

    return writeConcern;
}

Status validateWriteConcern(OperationContext* opCtx, const WriteConcernOptions& writeConcern) {
    if (writeConcern.syncMode == WriteConcernOptions::SyncMode::JOURNAL &&
        opCtx->getServiceContext()->getStorageEngine()->isEphemeral()) {
        return Status(ErrorCodes::BadValue,
                      "cannot use 'j' option when a host does not have journaling enabled");
    }

    if (!repl::ReplicationCoordinator::get(opCtx)->getSettings().isReplSet()) {
        if (holds_alternative<int64_t>(writeConcern.w) && get<int64_t>(writeConcern.w) > 1) {
            return Status(ErrorCodes::BadValue, "cannot use 'w' > 1 when a host is not replicated");
        }

        if (writeConcern.hasCustomWriteMode()) {
            return Status(
                ErrorCodes::BadValue,
                fmt::format("cannot use non-majority 'w' mode \"{}\" when a host is not a "
                            "member of a replica set",
                            get<std::string>(writeConcern.w)));
        }
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
    if (replCoord->getConfig().getCountOfVotingMembers() == 1) {
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
    // If we are in a direct client that's holding a global lock, then this means it is illegal to
    // wait for write concern. This is fine, since the outer operation should have handled waiting
    // for write concern.
    if (opCtx->getClient()->isInDirectClient() &&
        shard_role_details::getLocker(opCtx)->isLocked()) {
        return Status::OK();
    }

    LOGV2_DEBUG(22549,
                2,
                "Waiting for write concern. OpTime: {replOpTime}, write concern: {writeConcern}",
                "replOpTime"_attr = replOpTime,
                "writeConcern"_attr = writeConcern.toBSON());

    // Add time waiting for write concern to CurOp.
    CurOp::get(opCtx)->beginWaitForWriteConcernTimer();
    ScopeGuard finishTiming([&] { CurOp::get(opCtx)->stopWaitForWriteConcernTimer(); });

    auto* const storageEngine = opCtx->getServiceContext()->getStorageEngine();
    auto const replCoord = repl::ReplicationCoordinator::get(opCtx);

    if (MONGO_unlikely(hangBeforeWaitingForWriteConcern.shouldFail()) &&
        !opCtx->getClient()->isInDirectClient()) {
        // Respecting this failpoint for internal clients prevents stepup from working properly.
        // This fail point pauses with an open snapshot on the oplog. Some tests pause on this fail
        // point prior to running replication rollback. This prevents the operation from being
        // killed and the snapshot being released. Hence, we release the snapshot here.
        shard_role_details::replaceRecoveryUnit(opCtx);

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
                if (!storageEngine->isEphemeral()) {
                    // This field has had a dummy value since MMAP went away. It is undocumented.
                    // Maintaining it so as not to cause unnecessary user pain across upgrades.
                    result->fsyncFiles = 1;
                } else {
                    // We only need to commit the journal if we're durable
                    JournalFlusher::get(opCtx)->waitForJournalFlush(opCtx);
                }
                break;
            }
            case WriteConcernOptions::SyncMode::JOURNAL:
                waitForNoOplogHolesIfNeeded(opCtx);
                // In most cases we only need to trigger a journal flush without waiting for it
                // to complete because waiting for replication with j:true already tracks the
                // durable point for all data-bearing nodes and thus is sufficient to guarantee
                // durability.
                //
                // One exception is for w:1 writes where we need to wait for the journal flush
                // to complete because we skip waiting for replication for w:1 writes. In fact
                // for multi-voter replica sets, durability of w:1 writes could be meaningless
                // because they may still be rolled back if the primary crashes. Single-voter
                // replica sets, however, can never rollback confirmed writes, thus durability
                // does matter in this case.
                if (!writeConcernWithPopulatedSyncMode.needToWaitForOtherNodes()) {
                    JournalFlusher::get(opCtx)->waitForJournalFlush(opCtx);
                } else {
                    JournalFlusher::get(opCtx)->triggerJournalFlush();
                }
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

    auto replStatus =
        replCoord->awaitReplication(opCtx, replOpTime, writeConcernWithPopulatedSyncMode);

    if (auto mockStatusForTest = _tryGetWCFailureFromFailPoint_ForTest(replOpTime, writeConcern)) {
        replStatus = *mockStatusForTest;
    }

    if (replStatus.status == ErrorCodes::WriteConcernTimeout) {
        gleWtimeoutsMetric.add(1);
        if (!writeConcern.getProvenance().isClientSupplied()) {
            gleDefaultWtimeoutsMetric.add(1);
        }
        result->err = "timeout";
        result->wTimedOut = true;
    }
    if (replStatus.status == ErrorCodes::UnsatisfiableWriteConcern) {
        if (!writeConcern.getProvenance().isClientSupplied()) {
            gleDefaultUnsatisfiableMetric.add(1);
        }
    }

    gleWtimeNumMetric.add(1);
    gleWTimeTotalMillisMetric.add(durationCount<Milliseconds>(replStatus.duration));
    result->wTime = durationCount<Milliseconds>(replStatus.duration);

    result->wcUsed = writeConcern;

    return replStatus.status;
}

}  // namespace mongo
