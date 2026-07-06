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

#include "mongo/db/s/migration_source_manager.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/client.h"
#include "mongo/db/database_name.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/chunk.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/ddl/commit_chunk_migration_gen.h"
#include "mongo/db/global_catalog/ddl/shard_metadata_util.h"
#include "mongo/db/global_catalog/ddl/sharding_recovery_service.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_shard_collection.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/read_concern.h"
#include "mongo/db/repl/change_stream_oplog_notification.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/s/auto_split_vector.h"
#include "mongo/db/s/chunk_operation_precondition_checks.h"
#include "mongo/db/s/migration_chunk_cloner_source.h"
#include "mongo/db/s/migration_coordinator.h"
#include "mongo/db/s/migration_coordinator_document_gen.h"
#include "mongo/db/s/migration_util.h"
#include "mongo/db/s/random_migration_testing_utils.h"
#include "mongo/db/s/range_deletion_util.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/shard_filtering_metadata_refresh.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/sharding_logging.h"
#include "mongo/db/sharding_environment/sharding_runtime_d_params_gen.h"
#include "mongo/db/sharding_environment/sharding_statistics.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog.h"
#include "mongo/db/timeseries/bucket_catalog/global_bucket_catalog.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/transaction/reclaimed_prepared_txn_tracker.h"
#include "mongo/db/write_concern.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"

#include <string>
#include <tuple>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kShardingMigration

namespace mongo {
namespace {

const auto msmForCsr = CollectionShardingRuntime::declareDecoration<MigrationSourceManager*>();

// Wait at most this much time for the recipient to catch up sufficiently so critical section can be
// entered
const Hours kMaxWaitToEnterCriticalSectionTimeout(6);
const char kWriteConcernField[] = "writeConcern";

/*
 * Calculates the max or min bound perform split+move in case the chunk in question is splittable.
 * If the chunk is not splittable, returns the bound of the existing chunk for the max or min.Finds
 * a max bound if needMaxBound is true and a min bound if forward is false.
 */
BSONObj computeOtherBound(OperationContext* opCtx,
                          const CollectionAcquisition& acquisition,
                          const BSONObj& min,
                          const BSONObj& max,
                          const ShardKeyPattern& skPattern,
                          const long long maxChunkSizeBytes,
                          bool needMaxBound) {
    auto [splitKeys, _] = autoSplitVector(
        opCtx, acquisition, skPattern.toBSON(), min, max, maxChunkSizeBytes, 1, needMaxBound);
    if (splitKeys.size()) {
        return std::move(splitKeys.front());
    }

    // During testing, we try to randomly find a split point 50% of the time (so long as this shard
    // is not draining) in order to improve testing with multi-chunk collections.
    if (MONGO_unlikely(
            globalFailPointRegistry().find("balancerShouldReturnRandomMigrations")->shouldFail()) &&
        !random_migration_testing_utils::isCurrentShardDraining(opCtx) &&
        opCtx->getClient()->getPrng().trueWithProbability(0.5)) {
        if (auto randomSplitPoint = random_migration_testing_utils::generateRandomSplitPoint(
                opCtx, acquisition, skPattern.toBSON(), min, max)) {
            LOGV2(10587400,
                  "Selected random split point for balancing",
                  "min"_attr = min,
                  "max"_attr = max,
                  "skey"_attr = skPattern,
                  "splitPoint"_attr = *randomSplitPoint);
            return *randomSplitPoint;
        }
    }

    return needMaxBound ? max : min;
}

MONGO_FAIL_POINT_DEFINE(moveChunkHangAtStep1);
MONGO_FAIL_POINT_DEFINE(moveChunkHangAtStep2);
MONGO_FAIL_POINT_DEFINE(moveChunkHangAtStep3);
MONGO_FAIL_POINT_DEFINE(moveChunkHangAtStep4);
MONGO_FAIL_POINT_DEFINE(moveChunkHangAtStep5);
MONGO_FAIL_POINT_DEFINE(moveChunkHangAtStep6);

MONGO_FAIL_POINT_DEFINE(failMigrationCommit);
MONGO_FAIL_POINT_DEFINE(hangBeforeEnteringCriticalSection);
MONGO_FAIL_POINT_DEFINE(hangBeforeLeavingCriticalSection);
MONGO_FAIL_POINT_DEFINE(migrationCommitNetworkError);
MONGO_FAIL_POINT_DEFINE(hangBeforePostMigrationCommitRefresh);

}  // namespace

MigrationSourceManager* MigrationSourceManager::get(const CollectionShardingRuntime& csr) {
    return msmForCsr(csr);
}

std::shared_ptr<MigrationChunkClonerSource> MigrationSourceManager::getCurrentCloner(
    const CollectionShardingRuntime& csr) {
    auto msm = get(csr);
    if (!msm)
        return nullptr;
    return msm->_cloneDriver;
}

std::unique_ptr<MigrationSourceManager> MigrationSourceManager::createMigrationSourceManager(
    OperationContext* opCtx,
    ShardsvrMoveRange&& request,
    WriteConcernOptions&& writeConcern,
    ConnectionString donorConnStr,
    HostAndPort recipientHost,
    ManagementModeEnum managementMode,
    UUID migrationId) {
    invariant(!shard_role_details::getLocker(opCtx)->isLocked());

    auto&& args = std::move(request);
    const auto& nss = args.getCommandParameter();

    LOGV2(22016,
          "Starting chunk migration donation",
          "requestParameters"_attr = redact(args.toBSON()));

    // Make sure the latest placement version is recovered as of the time of the invocation of the
    // command.
    uassertStatusOK(
        FilteringMetadataCache::get(opCtx)->onShardVersionMismatch(opCtx, nss, boost::none));

    // Only the legacy path needs to drain migrations.
    // On the authoritative path, migrations are already blocked and are drained during the setFCV
    // upgrade.
    if (managementMode == ManagementModeEnum::kStandalone) {
        // Complete any unfinished migration pending recovery
        migrationutil::drainMigrationsPendingRecovery(opCtx);

        // Since the moveChunk command is holding the ActiveMigrationRegistry and we just drained
        // all migrations pending recovery, now there cannot be any document in
        // config.migrationCoordinators.
        PersistentTaskStore<MigrationCoordinatorDocument> store(
            NamespaceString::kMigrationCoordinatorsNamespace);
        invariant(store.count(opCtx) == 0);
    }

    // Compute the max or min bound in case only one is set (moveRange)
    if (!args.getMax().has_value() || !args.getMin().has_value()) {
        auto acquisition =
            acquireCollection(opCtx,
                              CollectionAcquisitionRequest::fromOpCtx(
                                  opCtx, nss, AcquisitionPrerequisites::OperationType::kRead),
                              MODE_IS);
        const auto metadata = [&]() {
            const auto scopedCsr =
                CollectionShardingRuntime::assertCollectionLockedAndAcquireShared(opCtx, nss);
            const auto metadata = checkCollectionIdentity(opCtx,
                                                          nss,
                                                          boost::none /* epoch */,
                                                          args.getCollectionTimestamp(),
                                                          acquisition.getCollectionPtr(),
                                                          *scopedCsr);
            return metadata;
        }();

        if (!args.getMax().has_value()) {
            const auto& min = *args.getMin();

            const auto cm = metadata.getChunkManager();
            const auto owningChunk = cm->findIntersectingChunkWithSimpleCollation(min);
            const auto max = computeOtherBound(opCtx,
                                               acquisition,
                                               min,
                                               owningChunk.getMax(),
                                               cm->getShardKeyPattern(),
                                               args.getMaxChunkSizeBytes(),
                                               true /* needMaxBound */);

            args.getMoveRangeRequestBase().setMax(max);
        } else if (!args.getMin().has_value()) {
            const auto& max = *args.getMax();

            const auto cm = metadata.getChunkManager();
            const auto owningChunk = getChunkForMaxBound(*cm, max);
            const auto min = computeOtherBound(opCtx,
                                               acquisition,
                                               owningChunk.getMin(),
                                               max,
                                               cm->getShardKeyPattern(),
                                               args.getMaxChunkSizeBytes(),
                                               false /* needMaxBound */);

            args.getMoveRangeRequestBase().setMin(min);
        }
    }
    return std::unique_ptr<MigrationSourceManager>(
        new MigrationSourceManager(opCtx,
                                   std::move(args),
                                   std::move(writeConcern),
                                   donorConnStr,
                                   recipientHost,
                                   managementMode,
                                   std::move(migrationId)));
}

MigrationSourceManager::MigrationSourceManager(OperationContext* opCtx,
                                               ShardsvrMoveRange&& request,
                                               WriteConcernOptions&& writeConcern,
                                               ConnectionString donorConnStr,
                                               HostAndPort recipientHost,
                                               ManagementModeEnum managementMode,
                                               UUID migrationId)
    : _opCtx(opCtx),
      _args(request),
      _writeConcern(writeConcern),
      _donorConnStr(std::move(donorConnStr)),
      _recipientHost(std::move(recipientHost)),
      _stats(ShardingStatistics::get(_opCtx)),
      _managementMode(managementMode),
      _migrationId(std::move(migrationId)),
      _critSecReason(migrationutil::makeCriticalSectionReasonForMoveRange(
          _args.getShardsvrMoveRangeRequest(), _migrationId)),
      _moveTimingHelper(_opCtx,
                        "from",
                        _args.getCommandParameter(),
                        _args.getMin(),
                        _args.getMax(),
                        6,  // Total number of steps
                        _args.getToShard(),
                        _args.getFromShard()),
      _collectionTimestamp(_args.getCollectionTimestamp()),
      _errMsg("") {
    // Since the MigrationSourceManager is registered on the CSR from the constructor, another
    // thread can get it and abort the migration (and get a reference to the completion promise's
    // future). When this happens, since we throw an exception from the constructor, the destructor
    // will not run, so we have to do complete it here, otherwise we get a BrokenPromise
    // TODO (SERVER-92531): Use existing clean up infrastructure when aborting in early stages
    ScopeGuard scopedGuard([&] { _completion.emplaceValue(); });

    _moveTimingHelper.done(1);
    moveChunkHangAtStep1.pauseWhileSet();

    // Snapshot the committed metadata from the time the migration starts and register the
    // MigrationSourceManager on the CSR.
    const auto [collectionMetadata, collectionUUID] = [&] {
        // TODO (SERVER-71444): Fix to be interruptible or document exception.
        UninterruptibleLockGuard noInterrupt(_opCtx);  // NOLINT.
        AutoGetCollection autoColl(_opCtx, nss(), MODE_IS);
        auto scopedCsr = CollectionShardingRuntime::acquireExclusive(opCtx, nss());

        auto metadata = checkCollectionIdentity(_opCtx,
                                                nss(),
                                                boost::none /* epoch */,
                                                _args.getCollectionTimestamp(),
                                                *autoColl,
                                                *scopedCsr);

        UUID collectionUUID = autoColl->uuid();

        // Atomically (still under the CSR lock held above) check whether migrations are allowed and
        // register the MigrationSourceManager on the CSR. This ensures that interruption due to the
        // change of allowMigrations or allowChunkOperations to false will properly serialize and
        // not allow any new MSMs to be running after the change.
        uassert(ErrorCodes::ConflictingOperationInProgress,
                fmt::format("Collection is undergoing changes so moveChunk is not allowed. "
                            "allowMigrations: {}, allowChunkOperations: {}",
                            metadata.allowMigrations(),
                            scopedCsr->allowChunkOperations()),
                metadata.allowMigrations() && scopedCsr->allowChunkOperations());

        _scopedRegisterer.emplace(this, *scopedCsr);

        return std::make_pair(std::move(metadata), std::move(collectionUUID));
    }();

    // Drain the execution/cancellation of any existing range deletion task overlapping with the
    // targeted range (a task issued by a previous migration may still be present when the migration
    // gets interrupted post-commit).
    const ChunkRange range(*_args.getMin(), *_args.getMax());
    const auto rangeDeletionWaitDeadline = opCtx->fastClockSource().now() +
        Milliseconds(drainOverlappingRangeDeletionsOnStartTimeoutMS.load());
    // CollectionShardingRuntime::waitForClean() allows to sync on tasks already registered on the
    // RangeDeleterService, but may miss pending ones in case this code runs after a failover. The
    // enclosing while loop allows to address such a gap.
    while (rangedeletionutil::checkForConflictingDeletions(opCtx, range, collectionUUID)) {
        LOGV2(9197000,
              "Migration start deferred because the requested range overlaps with one or more "
              "ranges already scheduled for deletion",
              logAttrs(nss()),
              "range"_attr = redact(range.toString()));

        auto status = CollectionShardingRuntime::waitForClean(
            opCtx, nss(), collectionUUID, range, rangeDeletionWaitDeadline);

        if (status.isOK() && opCtx->fastClockSource().now() >= rangeDeletionWaitDeadline) {
            status = Status(
                ErrorCodes::ExceededTimeLimit,
                "Failed to start new migration - a conflicting range deletion is still pending");
        }

        uassertStatusOK(status);

        // If the filtering metadata was cleared while the range deletion task was ongoing, then
        // 'waitForClean' would return immediately even though there really is an ongoing range
        // deletion task. For that case, we loop again until there is no conflicting task in
        // config.rangeDeletions
        opCtx->sleepFor(Milliseconds(1000));
    }

    checkShardKeyPattern(
        _opCtx, nss(), collectionMetadata, ChunkRange(*_args.getMin(), *_args.getMax()));
    checkRangeWithinChunk(
        _opCtx, nss(), collectionMetadata, ChunkRange(*_args.getMin(), *_args.getMax()));

    _collectionUUID = collectionUUID;

    _chunkVersion = collectionMetadata.getChunkManager()
                        ->findIntersectingChunkWithSimpleCollation(*_args.getMin())
                        .getLastmod();

    _moveTimingHelper.done(2);
    moveChunkHangAtStep2.pauseWhileSet();
    scopedGuard.dismiss();
}

MigrationSourceManager::~MigrationSourceManager() {
    // In standalone mode every path clears _cloneDriver before destruction (error paths via
    // _cleanupOnError(), the success path via _cleanup()), so the invariant below holds.
    //
    // The MoveRangeCoordinator splits migrate() and commit() across a persistence boundary, so a
    // stepdown can destroy the MSM without finishCommit() having run. A still-set _cloneDriver here
    // therefore always means an interruption: log the error and run a best-effort cleanup (the
    // coordinator's recovery drives the migration to a terminal state if this fails). Note
    // _cleanupOnError() must not run on this path - see the tassert there.
    if (_managementMode == ManagementModeEnum::kMoveRangeCoordinator) {
        if (_cloneDriver) {
            if (_state < kCommittingOnConfig) {
                _logMoveChunkErrorToChangelog();
            } else if (_commitRecorded) {
                _moveTimingHelper.done(6);
            }
            auto ignored = _cleanup(false);
        }
    } else {
        invariant(!_cloneDriver);
    }
    _stats.totalDonorMoveChunkTimeMillis.addAndFetch(_entireOpTimer.millis());

    if (_state == kDone) {
        _completion.emplaceValue();
    } else {
        std::string errMsg = "Migration not completed";
        if (_coordinator) {
            const auto& migrationId = _coordinator->getMigrationId();
            errMsg = str::stream() << "Migration " << migrationId << " not completed";
        }
        auto status = Status{ErrorCodes::Interrupted, errMsg};
        _completion.setError(status);
    }
}

void MigrationSourceManager::startClone() {
    invariant(!shard_role_details::getLocker(_opCtx)->isLocked());
    invariant(_state == kCreated);
    ScopeGuard scopedGuard([&] {
        // The MoveRangeCoordinator drives cleanup through its own phase flow, so the standalone
        // error cleanup must not run on that path.
        if (_managementMode != ManagementModeEnum::kMoveRangeCoordinator) {
            _cleanupOnError();
        }
    });
    _stats.countDonorMoveChunkStarted.addAndFetch(1);

    auto moveChunkDetails = BSON("min" << *_args.getMin() << "max" << *_args.getMax() << "from"
                                       << _args.getFromShard() << "to" << _args.getToShard());
    auto logChangeStatus = ShardingLogging::get(_opCtx)->logChangeChecked(
        _opCtx, "moveChunk.start", nss(), moveChunkDetails, defaultMajorityWriteConcernDoNotUse());
    withChangelogErrMsg(logChangeStatus.reason(), [&] { uassertStatusOK(logChangeStatus); });

    _cloneAndCommitTimer.reset();

    auto replCoord = repl::ReplicationCoordinator::get(_opCtx);
    withChangelogErrMsg("Command can only be run on replica sets", [&] {
        uassert(9992000,
                "Command can only be run on replica sets",
                replCoord->getSettings().isReplSet());
    });

    auto supportsPreservingPreparedTxnInPreciseCheckpoints =
        rss::ReplicatedStorageService::get(_opCtx->getServiceContext())
            .getPersistenceProvider()
            .supportsPreservingPreparedTxnInPreciseCheckpoints();

    if (supportsPreservingPreparedTxnInPreciseCheckpoints) {
        auto preparedTxnResolved =
            ReclaimedPreparedTxnTracker::get(_opCtx)->onAllReclaimedPreparedTxnsResolved();
        if (!preparedTxnResolved.isReady()) {
            LOGV2(11374000,
                  "Waiting for reclaimed prepared transactions from startup recovery to commit or "
                  "abort before starting migration cloning.",
                  logAttrs(nss()),
                  "details"_attr = moveChunkDetails);

            const auto deadline = _opCtx->fastClockSource().now() +
                Milliseconds(chunkMigrationWaitForReclaimedPreparedTxnsMaxWaitMS.load());
            const auto guard = _opCtx->makeDeadlineGuard(deadline, ErrorCodes::ExceededTimeLimit);

            _stats.chunkMigrationWaitedOnReclaimedPreparedTxns.addAndFetch(1);
            Timer waitTimer;
            ScopeGuard recordWaitTime([&] {
                _stats.chunkMigrationWaitForReclaimedPreparedTxnsMillis.addAndFetch(
                    waitTimer.millis());
            });

            // We need to wait because prepared transactions reclaimed from a precise checkpoint as
            // part of startup recovery can not set up an onCommit handler to send transaction
            // operations to chunk migration's internal buffers after cloning starts. As a result we
            // need to wait on those reclaimed prepared transactions to complete and for their
            // results to be transferred as part of initial cloning, otherwise we can lose data.
            preparedTxnResolved.get(_opCtx);

            LOGV2(
                11374001,
                "Finished waiting for all reclaimed prepared transactions from startup recovery to "
                "commit or abort. Starting migration cloning.",
                logAttrs(nss()),
                "details"_attr = moveChunkDetails);
        }

        preparedTxnResolved.get(_opCtx);
    }

    {
        const auto metadata = _getCurrentMetadataAndCheckForConflictingErrors();

        auto scopedCsr = CollectionShardingRuntime::acquireExclusive(_opCtx, nss());

        // Having the metadata manager registered on the collection sharding state is what
        // indicates that a chunk on that collection is being migrated to the OpObservers. With
        // an active migration, write operations require the cloner to be present in order to
        // track changes to the chunk which needs to be transmitted to the recipient.
        _cloneDriver = std::make_shared<MigrationChunkClonerSource>(
            _opCtx, _args, _writeConcern, metadata.getKeyPattern(), _donorConnStr, _recipientHost);

        // Remember the donor's shard version before the migration. The coordinator persists it so
        // the global-catalog commit can be re-sent from durable state after a failover.
        _donorShardVersionPreMigration = metadata.getShardPlacementVersion();

        _coordinator.emplace(_migrationId,
                             _cloneDriver->getSessionId(),
                             _args.getFromShard(),
                             _args.getToShard(),
                             nss(),
                             *_collectionUUID,
                             ChunkRange(*_args.getMin(), *_args.getMax()),
                             *_chunkVersion,
                             KeyPattern(metadata.getKeyPattern()),
                             metadata.getShardPlacementVersion(),
                             _args.getWaitForDelete(),
                             _managementMode);

        _state = kCloning;
    }

    {
        const repl::ReadConcernArgs readConcernArgs(replCoord->getMyLastAppliedOpTime(),
                                                    repl::ReadConcernLevel::kLocalReadConcern);
        auto readConcernStatus = waitForReadConcern(_opCtx, readConcernArgs, DatabaseName(), false);
        withChangelogErrMsg(readConcernStatus.reason(),
                            [&] { uassertStatusOK(readConcernStatus); });

        setPrepareConflictBehaviorForReadConcern(
            _opCtx, readConcernArgs, PrepareConflictBehavior::kEnforce);
    }

    _coordinator->startMigration(_opCtx);

    // The authoritative path (MoveRangeCoordinator) installs the post-migration metadata into the
    // shard catalog directly, so the recipient does not need to force a filtering-metadata refresh
    // when it starts receiving the chunk.
    // TODO (SERVER-127253): Remove this once v9.0 branches out.
    const bool isAuthoritative = _managementMode == ManagementModeEnum::kMoveRangeCoordinator;
    auto startCloneStatus = _cloneDriver->startClone(_opCtx,
                                                     _coordinator->getMigrationId(),
                                                     _coordinator->getLsid(),
                                                     _coordinator->getTxnNumber(),
                                                     isAuthoritative);
    withChangelogErrMsg(startCloneStatus.reason(), [&] { uassertStatusOK(startCloneStatus); });

    // Refresh the collection routing information after starting the clone driver to have a
    // stable view on whether the recipient is already owning other chunks of the collection.
    {
        auto collectionPlacementInfoStatus =
            Grid::get(_opCtx)->catalogCache()->getCollectionPlacementInfoWithRefresh(_opCtx, nss());

        withChangelogErrMsg(collectionPlacementInfoStatus.getStatus().reason(),
                            [&] { uassertStatusOK(collectionPlacementInfoStatus); });
        const auto cm = collectionPlacementInfoStatus.getValue();

        // If the chunk migration will cause the collection placement to be extended to a new
        // shard, persist the state so that it can be reported to change stream readers once the
        // operation gets committed.
        if (!cm.getVersion(_opCtx, _args.getToShard()).isSet()) {
            _coordinator->setTransfersFirstCollectionChunkToRecipient(_opCtx, true);
        }
    }

    _moveTimingHelper.done(3);
    moveChunkHangAtStep3.pauseWhileSet();
    scopedGuard.dismiss();
}

void MigrationSourceManager::awaitToCatchUp() {
    invariant(!shard_role_details::getLocker(_opCtx)->isLocked());
    invariant(_state == kCloning);
    ScopeGuard scopedGuard([&] {
        // The MoveRangeCoordinator drives cleanup through its own phase flow, so the standalone
        // error cleanup must not run on that path.
        if (_managementMode != ManagementModeEnum::kMoveRangeCoordinator) {
            _cleanupOnError();
        }
    });
    _stats.totalDonorChunkCloneTimeMillis.addAndFetch(_cloneAndCommitTimer.millis());
    _cloneAndCommitTimer.reset();

    // Block until the cloner deems it appropriate to enter the critical section.
    auto criticalSectionStatus = _cloneDriver->awaitUntilCriticalSectionIsAppropriate(
        _opCtx, kMaxWaitToEnterCriticalSectionTimeout);
    withChangelogErrMsg(criticalSectionStatus.reason(),
                        [&] { uassertStatusOK(criticalSectionStatus); });

    _state = kCloneCaughtUp;
    _moveTimingHelper.done(4);
    moveChunkHangAtStep4.pauseWhileSet(_opCtx);
    scopedGuard.dismiss();
}

void MigrationSourceManager::enterCriticalSection() {
    invariant(!shard_role_details::getLocker(_opCtx)->isLocked());
    invariant(_state == kCloneCaughtUp);
    ScopeGuard scopedGuard([&] {
        // The MoveRangeCoordinator drives cleanup through its own phase flow, so the standalone
        // error cleanup must not run on that path.
        if (_managementMode != ManagementModeEnum::kMoveRangeCoordinator) {
            _cleanupOnError();
        }
    });
    _stats.totalDonorChunkCloneTimeMillis.addAndFetch(_cloneAndCommitTimer.millis());
    _cloneAndCommitTimer.reset();

    hangBeforeEnteringCriticalSection.pauseWhileSet();

    LOGV2_DEBUG_OPTIONS(4817402,
                        2,
                        {logv2::LogComponent::kShardMigrationPerf},
                        "Starting critical section",
                        logAttrs(nss()),
                        "migrationId"_attr = _coordinator->getMigrationId());

    if (_managementMode == ManagementModeEnum::kMoveRangeCoordinator) {
        ShardingRecoveryService::get(_opCtx)->acquireRecoverableCriticalSectionBlockWrites(
            _opCtx,
            nss(),
            _critSecReason,
            defaultMajorityWriteConcernDoNotUse(),
            false /* clearShardCatalogCache */,
            Milliseconds(migrationLockAcquisitionMaxWaitMS.load()));
    } else {
        _critSec.emplace(_opCtx, nss(), _critSecReason);

        // Signal secondaries that the critical section has been entered, so they refresh their
        // routing table on next access and block behind the critical section. This preserves causal
        // consistency: a stale mongos cannot read secondary data at a cluster time that already
        // includes the migration commit. The write must happen after the critSec flag is set so the
        // refresh stalls behind the flag.
        auto shardCollectionsEntryUpdateStatus = shardmetadatautil::updateShardCollectionsEntry(
            _opCtx,
            BSON(ShardCollectionType::kNssFieldName
                 << NamespaceStringUtil::serialize(nss(), SerializationContext::stateDefault())),
            BSON("$inc" << BSON(ShardCollectionType::kEnterCriticalSectionCounterFieldName << 1)),
            false /*upsert*/);
        withChangelogErrMsg(shardCollectionsEntryUpdateStatus.reason(), [&] {
            uassertStatusOKWithContext(shardCollectionsEntryUpdateStatus,
                                       "Persist critical section signal for secondaries");
        });
    }

    _state = kCriticalSection;

    LOGV2(22017,
          "Migration successfully entered critical section",
          logAttrs(nss()),
          "migrationId"_attr = _coordinator->getMigrationId());

    scopedGuard.dismiss();
}

void MigrationSourceManager::commitChunkOnRecipient() {
    invariant(!shard_role_details::getLocker(_opCtx)->isLocked());
    invariant(_state == kCriticalSection);
    ScopeGuard scopedGuard([&] {
        // The MoveRangeCoordinator drives cleanup and migration recovery itself, so neither the
        // error cleanup nor the legacy async recovery must run here for that path.
        if (_managementMode != ManagementModeEnum::kMoveRangeCoordinator) {
            _cleanupOnError();

            migrationutil::asyncRecoverMigrationUntilSuccessOrStepDown(_opCtx,
                                                                       _args.getCommandParameter())
                .thenRunOn(Grid::get(_opCtx)->getExecutorPool()->getFixedExecutor())
                .getAsync([](auto) {});
        }
    });

    // Tell the recipient shard to fetch the latest changes. The legacy path must refresh its
    // filtering metadata when releasing the critical section; the authoritative path must not,
    // because it installs the post-migration metadata into the shard catalog directly.
    const bool clearShardCatalogCache =
        _managementMode != ManagementModeEnum::kMoveRangeCoordinator;
    auto commitCloneStatus = _cloneDriver->commitClone(_opCtx, clearShardCatalogCache);

    if (MONGO_unlikely(failMigrationCommit.shouldFail()) && commitCloneStatus.isOK()) {
        commitCloneStatus = {ErrorCodes::InternalError,
                             "Failing _recvChunkCommit due to failpoint."};
    }

    withChangelogErrMsg(commitCloneStatus.getStatus().reason(), [&] {
        uassertStatusOKWithContext(commitCloneStatus, "commit clone failed");
    });

    _recipientCloneCounts = commitCloneStatus.getValue()["counts"].Obj().getOwned();

    _state = kCloneCompleted;
    _moveTimingHelper.done(5);
    moveChunkHangAtStep5.pauseWhileSet();
    scopedGuard.dismiss();
}

void MigrationSourceManager::_buildCommitChunkMigrationRequest(BSONObjBuilder* builder,
                                                               const ChunkVersion& collVersion,
                                                               bool isAuthoritative) {
    auto migratedChunk = MigratedChunkType(*_chunkVersion, *_args.getMin(), *_args.getMax());
    CommitChunkMigrationRequest request(
        nss(), _args.getFromShard(), _args.getToShard(), migratedChunk, collVersion);
    // Tell the config server which commit path to run. The legacy path leaves this unset.
    if (isAuthoritative) {
        request.setAuthoritative(true);
    }
    request.serialize(builder);
    builder->append(kWriteConcernField, defaultMajorityWriteConcernDoNotUse().toBSON());
}

void MigrationSourceManager::commitChunkMetadataOnConfig() {
    invariant(!shard_role_details::getLocker(_opCtx)->isLocked());
    invariant(_state == kCloneCompleted);
    tassert(12795302,
            "commitChunkMetadataOnConfig must not run on the MoveRangeCoordinator path",
            _managementMode != ManagementModeEnum::kMoveRangeCoordinator);

    ScopeGuard scopedGuard([&] {
        _cleanupOnError();
        migrationutil::asyncRecoverMigrationUntilSuccessOrStepDown(_opCtx, nss())
            .thenRunOn(Grid::get(_opCtx)->getExecutorPool()->getFixedExecutor())
            .getAsync([](auto) {});
    });

    // If we have chunks left on the FROM shard, bump the version of one of them as well. This
    // will change the local collection major version, which indicates to other processes that
    // the chunk metadata has changed and they should refresh.
    BSONObjBuilder builder;

    {
        const auto metadata = _getCurrentMetadataAndCheckForConflictingErrors();
        _buildCommitChunkMigrationRequest(
            &builder, metadata.getCollPlacementVersion(), false /* isAuthoritative */);
    }

    // Read operations must begin to wait on the critical section just before we send the commit
    // operation to the config server. The coordinator path never reaches this function (see the
    // tassert above); it promotes the recoverable critical section via
    // promoteCriticalSectionToBlockReads() instead.
    _critSec->enterCommitPhase();

    _state = kCommittingOnConfig;

    Timer t;

    auto commitChunkMigrationResponse =
        Grid::get(_opCtx)->shardRegistry()->getConfigShard()->runCommand(
            _opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            DatabaseName::kAdmin,
            builder.obj(),
            Shard::RetryPolicy::kIdempotent);

    if (MONGO_unlikely(migrationCommitNetworkError.shouldFail())) {
        commitChunkMigrationResponse = Status(
            ErrorCodes::HostUnreachable, "Failpoint 'migrationCommitNetworkError' generated error");
    }

    Status migrationCommitStatus =
        Shard::CommandResponse::getEffectiveStatus(commitChunkMigrationResponse);

    if (!migrationCommitStatus.isOK()) {
        {
            withChangelogErrMsg("Failed to acquire exclusive lock", [&] {
                auto scopedCsr = CollectionShardingRuntime::acquireExclusive(_opCtx, nss());
                scopedCsr->clearCollectionMetadata(_opCtx);
            });
        }
        scopedGuard.dismiss();
        auto ignored = _cleanup(false);
        migrationutil::asyncRecoverMigrationUntilSuccessOrStepDown(_opCtx, nss())
            .thenRunOn(Grid::get(_opCtx)->getExecutorPool()->getFixedExecutor())
            .getAsync([](auto) {});
        uassertStatusOK(migrationCommitStatus);
    }

    // Asynchronously tell the recipient to release its critical section
    _coordinator->launchReleaseRecipientCriticalSection(_opCtx, true);

    hangBeforePostMigrationCommitRefresh.pauseWhileSet();

    try {
        LOGV2_DEBUG_OPTIONS(4817404,
                            2,
                            {logv2::LogComponent::kShardMigrationPerf},
                            "Starting post-migration commit refresh on the shard",
                            logAttrs(nss()),
                            "migrationId"_attr = _coordinator->getMigrationId());

        FilteringMetadataCache::get(_opCtx)->forceCollectionMetadataRefresh_DEPRECATED(_opCtx,
                                                                                       nss());
        FilteringMetadataCache::get(_opCtx)->waitForCollectionFlush(_opCtx, nss());

        LOGV2_DEBUG_OPTIONS(4817405,
                            2,
                            {logv2::LogComponent::kShardMigrationPerf},
                            "Finished post-migration commit refresh on the shard",
                            logAttrs(nss()),
                            "migrationId"_attr = _coordinator->getMigrationId());
    } catch (const DBException& ex) {
        LOGV2_DEBUG_OPTIONS(4817410,
                            2,
                            {logv2::LogComponent::kShardMigrationPerf},
                            "Finished post-migration commit refresh on the shard with error",
                            logAttrs(nss()),
                            "migrationId"_attr = _coordinator->getMigrationId(),
                            "error"_attr = redact(ex));
        {
            withChangelogErrMsg("Failed to acquire exclusive lock", [&] {
                auto scopedCsr = CollectionShardingRuntime::acquireExclusive(_opCtx, nss());
                scopedCsr->clearCollectionMetadata(_opCtx);
            });
        }
        scopedGuard.dismiss();
        auto ignored = _cleanup(false);
        throw;
    }

    // Migration succeeded

    const auto refreshedMetadata = _getCurrentMetadataAndCheckForConflictingErrors();
    {
        // Emit an oplog entry about the completion of the migration
        const bool noMoreCollectionChunksOnDonor =
            !refreshedMetadata.getChunkManager()->getVersion(_opCtx, _args.getFromShard()).isSet();
        notifyChangeStreamsOnChunkMigrated(
            _opCtx,
            nss(),
            _collectionUUID,
            _args.getFromShard(),
            _args.getToShard(),
            noMoreCollectionChunksOnDonor,
            _coordinator->getTransfersFirstCollectionChunkToRecipient());
    }

    LOGV2(22018,
          "Migration succeeded and updated collection placement version",
          "updatedCollectionPlacementVersion"_attr = refreshedMetadata.getCollPlacementVersion(),
          logAttrs(nss()),
          "migrationId"_attr = _coordinator->getMigrationId());

    // If the migration has succeeded, clear the BucketCatalog so that the buckets that got migrated
    // out are no longer updatable.
    if (refreshedMetadata.getChunkManager()->isTimeseriesCollection()) {
        auto& bucketCatalog =
            timeseries::bucket_catalog::GlobalBucketCatalog::get(_opCtx->getServiceContext());
        clear(bucketCatalog, _collectionUUID.get());
    }

    _coordinator->setMigrationDecision(DecisionEnum::kCommitted);

    hangBeforeLeavingCriticalSection.pauseWhileSet();

    scopedGuard.dismiss();

    _stats.totalCriticalSectionCommitTimeMillis.addAndFetch(t.millis());

    LOGV2(6107801,
          "Exiting commit critical section",
          logAttrs(nss()),
          "migrationId"_attr = _coordinator->getMigrationId(),
          "durationMillis"_attr = t.millis());

    // Exit the critical section and ensure that all the necessary state is fully persisted
    // before scheduling orphan cleanup.
    uassertStatusOK(_cleanup(true));

    ShardingLogging::get(_opCtx)->logChange(
        _opCtx,
        "moveChunk.commit",
        nss(),
        BSON("min" << *_args.getMin() << "max" << *_args.getMax() << "from" << _args.getFromShard()
                   << "to" << _args.getToShard() << "counts" << *_recipientCloneCounts),
        defaultMajorityWriteConcernDoNotUse());

    const ChunkRange range(*_args.getMin(), *_args.getMax());

    std::string orphanedRangeCleanUpErrMsg = str::stream()
        << "Moved chunks successfully but failed to clean up " << nss().toStringForErrorMsg()
        << " range " << redact(range.toString()) << " due to: ";

    if (_args.getWaitForDelete()) {
        LOGV2(22019,
              "Waiting for migration cleanup after chunk commit",
              logAttrs(nss()),
              "migrationId"_attr = _coordinator->getMigrationId(),
              "range"_attr = redact(range.toString()));

        Status deleteStatus = _cleanupCompleteFuture
            ? _cleanupCompleteFuture->getNoThrow(_opCtx)
            : Status(ErrorCodes::Error(5089002),
                     "Not honouring the 'waitForDelete' request because migration coordinator "
                     "cleanup didn't succeed");
        if (!deleteStatus.isOK()) {
            uasserted(ErrorCodes::OrphanedRangeCleanUpFailed,
                      orphanedRangeCleanUpErrMsg + redact(deleteStatus));
        }
    }

    _moveTimingHelper.done(6);
    moveChunkHangAtStep6.pauseWhileSet();
}

void MigrationSourceManager::promoteCriticalSectionToBlockReads() {
    tassert(12795303,
            "promoteCriticalSectionToBlockReads is only valid on the MoveRangeCoordinator path",
            _managementMode == ManagementModeEnum::kMoveRangeCoordinator);
    tassert(12795304,
            "promoteCriticalSectionToBlockReads requires the clone to have completed",
            _state == kCloneCompleted);
    // Read operations must begin to wait on the critical section just before we send the commit
    // operation to the config server.
    ShardingRecoveryService::get(_opCtx)->promoteRecoverableCriticalSectionToBlockAlsoReads(
        _opCtx,
        nss(),
        _critSecReason,
        defaultMajorityWriteConcernDoNotUse(),
        Milliseconds(migrationLockAcquisitionMaxWaitMS.load()));
}

void MigrationSourceManager::markCommitInProgress() {
    tassert(12953601,
            "markCommitInProgress is only valid on the MoveRangeCoordinator path",
            _managementMode == ManagementModeEnum::kMoveRangeCoordinator);
    tassert(12953602,
            "markCommitInProgress requires the clone to have completed",
            _state == kCloneCompleted);
    tassert(12953603,
            "markCommitInProgress requires the chunk cloner to have finished",
            _cloneDriver && _cloneDriver->isDone());

    // The commit is about to be sent. From here on the migration may already be committed, so move
    // past kCloneCompleted: cleanup keys off this state and must not treat it as a clean abort.
    _state = kCommittingOnConfig;
}

void MigrationSourceManager::recordCommitSuccess(OperationContext* opCtx) {
    tassert(12795307,
            "recordCommitSuccess is only valid on the MoveRangeCoordinator path",
            _managementMode == ManagementModeEnum::kMoveRangeCoordinator);

    // Idempotent across same-term retries of the commit phase.
    if (_commitRecorded) {
        return;
    }

    tassert(12795308,
            "recordCommitSuccess requires the config commit to be in progress",
            _state == kCommittingOnConfig);

    // The config commit has happened but the donor's metadata is not yet refreshed/installed (a
    // later coordinator phase does that). This mirrors the standalone pause point that tests use to
    // exercise causally-consistent reads against still-stale donor metadata.
    hangBeforePostMigrationCommitRefresh.pauseWhileSet();

    // Read the post-commit placement from the catalog cache rather than refreshing the filtering
    // metadata: the authoritative local shard catalog is installed later, while the critical
    // section is still held.
    const auto cm = uassertStatusOK(
        Grid::get(opCtx)->catalogCache()->getCollectionPlacementInfoWithRefresh(opCtx, nss()));

    // If the migration has succeeded, clear the BucketCatalog so that the buckets that got migrated
    // out are no longer updatable.
    if (cm.isTimeseriesCollection()) {
        auto& bucketCatalog =
            timeseries::bucket_catalog::GlobalBucketCatalog::get(_opCtx->getServiceContext());
        clear(bucketCatalog, _collectionUUID.get());
    }

    _coordinator->setMigrationDecision(DecisionEnum::kCommitted);
    _commitRecorded = true;

    // Record the commit in the sharding changelog for auditing and tooling.
    ShardingLogging::get(_opCtx)->logChange(
        _opCtx,
        "moveChunk.commit",
        nss(),
        BSON("min" << *_args.getMin() << "max" << *_args.getMax() << "from" << _args.getFromShard()
                   << "to" << _args.getToShard() << "counts" << *_recipientCloneCounts),
        defaultMajorityWriteConcernDoNotUse());
}

void MigrationSourceManager::finishCommit() {
    tassert(12795309,
            "finishCommit is only valid on the MoveRangeCoordinator path",
            _managementMode == ManagementModeEnum::kMoveRangeCoordinator);

    // The kFinalizeMigration phase may be retried; _cleanup() can only run once (it asserts
    // _state != kDone and moves the state to kDone). If it already ran, there is nothing more to do
    // here.
    if (_state == kDone) {
        return;
    }

    hangBeforeLeavingCriticalSection.pauseWhileSet();

    // Whether the migration reached the config commit, captured before _cleanup() moves the state
    // to kDone. MoveTimingHelper::done() requires strictly sequential steps, so step 6 may only be
    // recorded on the committed path; on the abort path earlier steps may have been skipped.
    const bool reachedConfigCommit = _state == kCommittingOnConfig;

    // On the abort path, record the migration error in the change log before cleaning up, mirroring
    // _cleanupOnError(). _errMsg, set by the failing migration step, is still populated here.
    if (!reachedConfigCommit) {
        _logMoveChunkErrorToChangelog();
    }

    // Complete the migration coordinator (releases the recipient critical section, schedules range
    // deletion, and forgets the coordinator document). The donor critical section was already
    // released earlier in the kFinalizeMigration phase. When the commit result was uncertain,
    // _coordinator has no in-memory decision, so this is a no-op and the coordinator drives
    // completion from the persisted decision instead.
    uassertStatusOK(_cleanup(true));

    // waitForDelete is only honoured when completion scheduled range deletion (i.e.
    // _cleanupCompleteFuture is set). On the abort path, or when completion was deferred to the
    // persisted-decision path, there is no future to wait on.
    if (_args.getWaitForDelete() && _cleanupCompleteFuture) {
        const ChunkRange range(*_args.getMin(), *_args.getMax());
        LOGV2(12795315,
              "Waiting for migration cleanup after chunk commit",
              logAttrs(nss()),
              "migrationId"_attr = _migrationId,
              "range"_attr = redact(range.toString()));

        Status deleteStatus = _cleanupCompleteFuture->getNoThrow(_opCtx);
        if (!deleteStatus.isOK()) {
            uasserted(ErrorCodes::OrphanedRangeCleanUpFailed,
                      str::stream()
                          << "Moved chunks successfully but failed to clean up "
                          << nss().toStringForErrorMsg() << " range " << redact(range.toString())
                          << " due to: " << redact(deleteStatus));
        }
    }

    // Mark the final migration step done and expose the end-of-commit pause point, so move-timing
    // and existing tests behave the same as the standalone path. Like
    // commitChunkMetadataOnConfig(), this only runs for a committed migration; the abort path does
    // not pause here.
    if (reachedConfigCommit) {
        _moveTimingHelper.done(6);
        moveChunkHangAtStep6.pauseWhileSet();
    }
}

void MigrationSourceManager::_logMoveChunkErrorToChangelog() {
    BSONObjBuilder logDetails;
    logDetails.append("min", *_args.getMin())
        .append("max", *_args.getMax())
        .append("from", _args.getFromShard())
        .append("to", _args.getToShard());

    if (!_errMsg.empty()) {
        logDetails.append("errmsg", _errMsg);
    }

    ShardingLogging::get(_opCtx)->logChange(_opCtx,
                                            "moveChunk.error",
                                            _args.getCommandParameter(),
                                            logDetails.obj(),
                                            defaultMajorityWriteConcernDoNotUse());
}

void MigrationSourceManager::_cleanupOnError() {
    tassert(12795316,
            "_cleanupOnError must not run on the MoveRangeCoordinator path; that path drives "
            "cleanup and error logging through the coordinator's kFinalizeMigration phase",
            _managementMode != ManagementModeEnum::kMoveRangeCoordinator);

    if (_state == kDone) {
        return;
    }

    _logMoveChunkErrorToChangelog();

    auto ignored = _cleanup(true);
}

template <typename F>
void MigrationSourceManager::withChangelogErrMsg(const std::string errMsg, F&& functionCall) {
    _errMsg = errMsg;
    std::forward<F>(functionCall)();
    _errMsg = "";
}

SharedSemiFuture<void> MigrationSourceManager::abort() {
    std::lock_guard<Client> lk(*_opCtx->getClient());
    _opCtx->markKilled();
    _stats.countDonorMoveChunkAbortConflictingIndexOperation.addAndFetch(1);

    return _completion.getFuture();
}

CollectionMetadata MigrationSourceManager::_getCurrentMetadataAndCheckForConflictingErrors() {
    auto metadata = [&] {
        const auto scopedCsr =
            CollectionShardingRuntime::acquireShared(_opCtx, _args.getCommandParameter());

        const auto optMetadata = scopedCsr->getCurrentMetadataIfKnown();
        withChangelogErrMsg(
            "The collection's sharding state was cleared by a concurrent operation", [&] {
                uassert(ErrorCodes::ConflictingOperationInProgress,
                        "The collection's sharding state was cleared by a concurrent operation",
                        optMetadata);
            });
        return *optMetadata;
    }();

    std::string conflictingOperationErrMsg = str::stream()
        << "The collection's timestamp has changed since the migration began. Expected "
           "timestamp: "
        << _collectionTimestamp.toStringPretty() << ", but found: "
        << (metadata.isSharded()
                ? metadata.getCollPlacementVersion().getTimestamp().toStringPretty()
                : "unsharded collection");
    withChangelogErrMsg(conflictingOperationErrMsg, [&] {
        uassert(ErrorCodes::ConflictingOperationInProgress,
                conflictingOperationErrMsg,
                metadata.isSharded() &&
                    _collectionTimestamp == metadata.getCollPlacementVersion().getTimestamp());
    });

    return metadata;
}

Status MigrationSourceManager::_cleanup(bool completeMigration) {
    invariant(_state != kDone);

    LOGV2_DEBUG_OPTIONS(12795301,
                        2,
                        {logv2::LogComponent::kShardMigrationPerf},
                        "Running cleanup",
                        "completeMigration"_attr = completeMigration);
    auto cleanupResult = Status::OK();

    auto cloneDriver = [&]() {
        auto scopedCsr = CollectionShardingRuntime::acquireExclusive(_opCtx, nss());
        if (_state != kCreated) {
            invariant(_cloneDriver);
        }
        return std::move(_cloneDriver);
    }();

    // Exit the migration critical section. For the MoveRangeCoordinator path the donor critical
    // section is a recoverable critical section released by the coordinator during its
    // kFinalizeMigration phase, so _cleanup() must not touch it here.
    if (_managementMode == ManagementModeEnum::kStandalone) {
        _critSec.reset();

        if (_state == kCriticalSection || _state == kCloneCompleted ||
            _state == kCommittingOnConfig) {
            LOGV2_DEBUG_OPTIONS(4817403,
                                2,
                                {logv2::LogComponent::kShardMigrationPerf},
                                "Finished critical section",
                                logAttrs(nss()),
                                "migrationId"_attr = _coordinator->getMigrationId());

            LOGV2(6107802,
                  "Finished critical section",
                  logAttrs(nss()),
                  "migrationId"_attr = _coordinator->getMigrationId(),
                  "durationMillis"_attr = _cloneAndCommitTimer.millis());
        }
    }

    // The cleanup operations below are potentially blocking or acquire other locks, so perform
    // them outside of the collection X lock.
    //
    // cancelClone() only signals the recipient to abort while the clone is still running. Once the
    // clone has finished (the commit phase), it just tears down donor-side cloner state.
    if (cloneDriver) {
        cloneDriver->cancelClone(_opCtx);
    }

    try {
        if (_state >= kCloning) {
            invariant(_coordinator);
            if (_state < kCommittingOnConfig) {
                _coordinator->setMigrationDecision(DecisionEnum::kAborted);
            }

            auto newClient =
                _opCtx->getServiceContext()->getService()->makeClient("MigrationCoordinator");
            AlternativeClientRegion acr(newClient);
            auto newOpCtxPtr = cc().makeOperationContext();
            auto newOpCtx = newOpCtxPtr.get();

            if (_state >= kCriticalSection && _state <= kCommittingOnConfig) {
                _stats.totalCriticalSectionTimeMillis.addAndFetch(_cloneAndCommitTimer.millis());
            }
            if (completeMigration) {
                // This can be called on an exception path after the OperationContext has been
                // interrupted, so use a new OperationContext. Note, it's valid to call
                // getServiceContext on an interrupted OperationContext.

                // Tell the recipient whether to refresh its filtering metadata when releasing its
                // critical section. The legacy path must refresh; the authoritative path must not,
                // because it installs the post-migration metadata into the shard catalog directly.
                const bool clearCatalogCache =
                    _managementMode != ManagementModeEnum::kMoveRangeCoordinator;

                _cleanupCompleteFuture =
                    _coordinator->completeMigration(newOpCtx, clearCatalogCache);
            }
        }

        _state = kDone;
    } catch (const DBException& ex) {
        LOGV2_WARNING(5089001,
                      "Failed to complete the migration",
                      "chunkMigrationRequestParameters"_attr = redact(_args.toBSON()),
                      "error"_attr = redact(ex),
                      logAttrs(nss()),
                      "migrationId"_attr = _coordinator->getMigrationId());
        if (_managementMode == ManagementModeEnum::kStandalone) {
            // Something went really wrong when completing the migration just unset the metadata and
            // let the next op to recover.
            auto scopedCsr = CollectionShardingRuntime::acquireExclusive(_opCtx, nss());
            scopedCsr->clearCollectionMetadata(_opCtx);
        }
        cleanupResult = ex.toStatus();
    }
    return cleanupResult;
}

BSONObj MigrationSourceManager::getMigrationStatusReport(
    const CollectionShardingRuntime::ScopedSharedCollectionShardingRuntime& scopedCsrLock) const {

    // Important: This method is being called from a thread other than the main one, however any
    // non-const fields accessed here (_cloneDriver) are written through the exclusive CSR lock.

    boost::optional<long long> sessionOplogEntriesToBeMigratedSoFar;
    boost::optional<long long> sessionOplogEntriesSkippedSoFarLowerBound;
    if (_cloneDriver) {
        sessionOplogEntriesToBeMigratedSoFar =
            _cloneDriver->getSessionOplogEntriesToBeMigratedSoFar();
        sessionOplogEntriesSkippedSoFarLowerBound =
            _cloneDriver->getSessionOplogEntriesSkippedSoFarLowerBound();
    }

    return migrationutil::makeMigrationStatusDocumentSource(
        _args.getCommandParameter(),
        _args.getFromShard(),
        _args.getToShard(),
        true,
        _args.getMin().value_or(BSONObj()),
        _args.getMax().value_or(BSONObj()),
        sessionOplogEntriesToBeMigratedSoFar,
        sessionOplogEntriesSkippedSoFarLowerBound);
}

MigrationSourceManager::ScopedRegisterer::ScopedRegisterer(MigrationSourceManager* msm,
                                                           CollectionShardingRuntime& csr)
    : _msm(msm) {
    invariant(nullptr == std::exchange(msmForCsr(csr), msm));
}

MigrationSourceManager::ScopedRegisterer::~ScopedRegisterer() {
    auto scopedCsr = CollectionShardingRuntime::acquireExclusive(_msm->_opCtx,
                                                                 _msm->_args.getCommandParameter());
    invariant(_msm == std::exchange(msmForCsr(*scopedCsr), nullptr));
}

}  // namespace mongo
