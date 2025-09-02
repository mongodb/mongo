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
#include "mongo/base/string_data.h"
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
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_shard_collection.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/local_catalog/shard_role_catalog/shard_filtering_metadata_refresh.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/read_concern.h"
#include "mongo/db/repl/change_stream_oplog_notification.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/auto_split_vector.h"
#include "mongo/db/s/chunk_operation_precondition_checks.h"
#include "mongo/db/s/migration_chunk_cloner_source.h"
#include "mongo/db/s/migration_coordinator.h"
#include "mongo/db/s/migration_coordinator_document_gen.h"
#include "mongo/db/s/migration_util.h"
#include "mongo/db/s/random_migration_testing_utils.h"
#include "mongo/db/s/range_deletion_util.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/sharding_logging.h"
#include "mongo/db/sharding_environment/sharding_statistics.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog.h"
#include "mongo/db/timeseries/bucket_catalog/global_bucket_catalog.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/write_concern.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

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
        opCtx->getClient()->getPrng().nextCanonicalDouble() < 0.5) {
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

MigrationSourceManager MigrationSourceManager::createMigrationSourceManager(
    OperationContext* opCtx,
    ShardsvrMoveRange&& request,
    WriteConcernOptions&& writeConcern,
    ConnectionString donorConnStr,
    HostAndPort recipientHost) {
    invariant(!shard_role_details::getLocker(opCtx)->isLocked());

    auto&& args = std::move(request);
    const auto& nss = args.getCommandParameter();

    LOGV2(22016,
          "Starting chunk migration donation",
          "requestParameters"_attr = redact(args.toBSON()));

    // Make sure the latest placement version is recovered as of the time of the invocation of the
    // command.
    uassertStatusOK(FilteringMetadataCache::get(opCtx)->onCollectionPlacementVersionMismatch(
        opCtx, nss, boost::none));

    // Complete any unfinished migration pending recovery
    {
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
    return MigrationSourceManager(
        opCtx, std::move(args), std::move(writeConcern), donorConnStr, recipientHost);
}

MigrationSourceManager::MigrationSourceManager(OperationContext* opCtx,
                                               ShardsvrMoveRange&& request,
                                               WriteConcernOptions&& writeConcern,
                                               ConnectionString donorConnStr,
                                               HostAndPort recipientHost)
    : _opCtx(opCtx),
      _args(request),
      _writeConcern(writeConcern),
      _donorConnStr(std::move(donorConnStr)),
      _recipientHost(std::move(recipientHost)),
      _stats(ShardingStatistics::get(_opCtx)),
      _critSecReason(BSON("command" << "moveChunk"
                                    << "fromShard" << _args.getFromShard() << "toShard"
                                    << _args.getToShard())),
      _moveTimingHelper(_opCtx,
                        "from",
                        _args.getCommandParameter(),
                        _args.getMin(),
                        _args.getMax(),
                        6,  // Total number of steps
                        _args.getToShard(),
                        _args.getFromShard()),
      _collectionTimestamp(_args.getCollectionTimestamp()) {
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
        auto scopedCsr =
            CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(opCtx, nss());

        auto metadata = checkCollectionIdentity(_opCtx,
                                                nss(),
                                                boost::none /* epoch */,
                                                _args.getCollectionTimestamp(),
                                                *autoColl,
                                                *scopedCsr);

        UUID collectionUUID = autoColl.getCollection()->uuid();

        // Atomically (still under the CSR lock held above) check whether migrations are allowed and
        // register the MigrationSourceManager on the CSR. This ensures that interruption due to the
        // change of allowMigrations to false will properly serialise and not allow any new MSMs to
        // be running after the change.
        uassert(ErrorCodes::ConflictingOperationInProgress,
                "Collection is undergoing changes so moveChunk is not allowed.",
                metadata.allowMigrations());

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
    invariant(!_cloneDriver);
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
    ScopeGuard scopedGuard([&] { _cleanupOnError(); });
    _stats.countDonorMoveChunkStarted.addAndFetch(1);

    uassertStatusOK(ShardingLogging::get(_opCtx)->logChangeChecked(
        _opCtx,
        "moveChunk.start",
        nss(),
        BSON("min" << *_args.getMin() << "max" << *_args.getMax() << "from" << _args.getFromShard()
                   << "to" << _args.getToShard()),
        defaultMajorityWriteConcernDoNotUse()));

    _cloneAndCommitTimer.reset();

    auto replCoord = repl::ReplicationCoordinator::get(_opCtx);
    uassert(
        9992000, "Command can only be run on replica sets", replCoord->getSettings().isReplSet());

    {
        const auto metadata = _getCurrentMetadataAndCheckForConflictingErrors();

        auto scopedCsr = CollectionShardingRuntime::acquireExclusive(_opCtx, nss());

        // Having the metadata manager registered on the collection sharding state is what indicates
        // that a chunk on that collection is being migrated to the OpObservers. With an active
        // migration, write operations require the cloner to be present in order to track changes to
        // the chunk which needs to be transmitted to the recipient.
        _cloneDriver = std::make_shared<MigrationChunkClonerSource>(
            _opCtx, _args, _writeConcern, metadata.getKeyPattern(), _donorConnStr, _recipientHost);

        _coordinator.emplace(_cloneDriver->getSessionId(),
                             _args.getFromShard(),
                             _args.getToShard(),
                             nss(),
                             *_collectionUUID,
                             ChunkRange(*_args.getMin(), *_args.getMax()),
                             *_chunkVersion,
                             KeyPattern(metadata.getKeyPattern()),
                             metadata.getShardPlacementVersion(),
                             _args.getWaitForDelete());

        _state = kCloning;
    }

    {
        const repl::ReadConcernArgs readConcernArgs(replCoord->getMyLastAppliedOpTime(),
                                                    repl::ReadConcernLevel::kLocalReadConcern);
        uassertStatusOK(waitForReadConcern(_opCtx, readConcernArgs, DatabaseName(), false));

        setPrepareConflictBehaviorForReadConcern(
            _opCtx, readConcernArgs, PrepareConflictBehavior::kEnforce);
    }

    _coordinator->startMigration(_opCtx);

    uassertStatusOK(_cloneDriver->startClone(_opCtx,
                                             _coordinator->getMigrationId(),
                                             _coordinator->getLsid(),
                                             _coordinator->getTxnNumber()));

    // Refresh the collection routing information after starting the clone driver to have a
    // stable view on whether the recipient is already owning other chunks of the collection.
    {
        const auto cm = uassertStatusOK(
            Grid::get(_opCtx)->catalogCache()->getCollectionPlacementInfoWithRefresh(_opCtx,
                                                                                     nss()));
        // If the chunk migration will cause the collection placement to be extended to a new shard,
        // persist the state so that it can be reported to change stream readers once the operation
        // gets committed.
        if (!cm.getVersion(_args.getToShard()).isSet()) {
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
    ScopeGuard scopedGuard([&] { _cleanupOnError(); });
    _stats.totalDonorChunkCloneTimeMillis.addAndFetch(_cloneAndCommitTimer.millis());
    _cloneAndCommitTimer.reset();

    // Block until the cloner deems it appropriate to enter the critical section.
    uassertStatusOK(_cloneDriver->awaitUntilCriticalSectionIsAppropriate(
        _opCtx, kMaxWaitToEnterCriticalSectionTimeout));

    _state = kCloneCaughtUp;
    _moveTimingHelper.done(4);
    moveChunkHangAtStep4.pauseWhileSet(_opCtx);
    scopedGuard.dismiss();
}

void MigrationSourceManager::enterCriticalSection() {
    invariant(!shard_role_details::getLocker(_opCtx)->isLocked());
    invariant(_state == kCloneCaughtUp);
    ScopeGuard scopedGuard([&] { _cleanupOnError(); });
    _stats.totalDonorChunkCloneTimeMillis.addAndFetch(_cloneAndCommitTimer.millis());
    _cloneAndCommitTimer.reset();

    hangBeforeEnteringCriticalSection.pauseWhileSet();

    LOGV2_DEBUG_OPTIONS(4817402,
                        2,
                        {logv2::LogComponent::kShardMigrationPerf},
                        "Starting critical section",
                        "migrationId"_attr = _coordinator->getMigrationId(),
                        logAttrs(nss()));

    _critSec.emplace(_opCtx, nss(), _critSecReason);

    _state = kCriticalSection;

    // Persist a signal to secondaries that we've entered the critical section. This is will cause
    // secondaries to refresh their routing table when next accessed, which will block behind the
    // critical section. This ensures causal consistency by preventing a stale mongos with a cluster
    // time inclusive of the migration config commit update from accessing secondary data.
    // Note: this write must occur after the critSec flag is set, to ensure the secondary refresh
    // will stall behind the flag.
    uassertStatusOKWithContext(
        shardmetadatautil::updateShardCollectionsEntry(
            _opCtx,
            BSON(ShardCollectionType::kNssFieldName
                 << NamespaceStringUtil::serialize(nss(), SerializationContext::stateDefault())),
            BSON("$inc" << BSON(ShardCollectionType::kEnterCriticalSectionCounterFieldName << 1)),
            false /*upsert*/),
        "Persist critical section signal for secondaries");

    LOGV2(22017,
          "Migration successfully entered critical section",
          "migrationId"_attr = _coordinator->getMigrationId(),
          logAttrs(nss()));

    scopedGuard.dismiss();
}

void MigrationSourceManager::commitChunkOnRecipient() {
    invariant(!shard_role_details::getLocker(_opCtx)->isLocked());
    invariant(_state == kCriticalSection);
    ScopeGuard scopedGuard([&] {
        _cleanupOnError();
        migrationutil::asyncRecoverMigrationUntilSuccessOrStepDown(_opCtx,
                                                                   _args.getCommandParameter())
            .thenRunOn(Grid::get(_opCtx)->getExecutorPool()->getFixedExecutor())
            .getAsync([](auto) {});
    });

    // Tell the recipient shard to fetch the latest changes.
    auto commitCloneStatus = _cloneDriver->commitClone(_opCtx);

    if (MONGO_unlikely(failMigrationCommit.shouldFail()) && commitCloneStatus.isOK()) {
        commitCloneStatus = {ErrorCodes::InternalError,
                             "Failing _recvChunkCommit due to failpoint."};
    }

    uassertStatusOKWithContext(commitCloneStatus, "commit clone failed");
    _recipientCloneCounts = commitCloneStatus.getValue()["counts"].Obj().getOwned();

    _state = kCloneCompleted;
    _moveTimingHelper.done(5);
    moveChunkHangAtStep5.pauseWhileSet();
    scopedGuard.dismiss();
}

void MigrationSourceManager::commitChunkMetadataOnConfig() {
    invariant(!shard_role_details::getLocker(_opCtx)->isLocked());
    invariant(_state == kCloneCompleted);

    ScopeGuard scopedGuard([&] {
        _cleanupOnError();
        migrationutil::asyncRecoverMigrationUntilSuccessOrStepDown(_opCtx, nss())
            .thenRunOn(Grid::get(_opCtx)->getExecutorPool()->getFixedExecutor())
            .getAsync([](auto) {});
    });

    // If we have chunks left on the FROM shard, bump the version of one of them as well. This will
    // change the local collection major version, which indicates to other processes that the chunk
    // metadata has changed and they should refresh.
    BSONObjBuilder builder;

    {
        const auto metadata = _getCurrentMetadataAndCheckForConflictingErrors();

        auto migratedChunk = MigratedChunkType(*_chunkVersion, *_args.getMin(), *_args.getMax());

        CommitChunkMigrationRequest request(nss(),
                                            _args.getFromShard(),
                                            _args.getToShard(),
                                            migratedChunk,
                                            metadata.getCollPlacementVersion());

        request.serialize(&builder);
        builder.append(kWriteConcernField, defaultMajorityWriteConcernDoNotUse().toBSON());
    }

    // Read operations must begin to wait on the critical section just before we send the commit
    // operation to the config server
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
            ErrorCodes::InternalError, "Failpoint 'migrationCommitNetworkError' generated error");
    }

    Status migrationCommitStatus =
        Shard::CommandResponse::getEffectiveStatus(commitChunkMigrationResponse);

    if (!migrationCommitStatus.isOK()) {
        {
            // TODO (SERVER-71444): Fix to be interruptible or document exception.
            UninterruptibleLockGuard noInterrupt(_opCtx);  // NOLINT.
            AutoGetCollection autoColl(_opCtx, nss(), MODE_IX);
            CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(_opCtx, nss())
                ->clearFilteringMetadata(_opCtx);
        }
        scopedGuard.dismiss();
        _cleanup(false);
        migrationutil::asyncRecoverMigrationUntilSuccessOrStepDown(_opCtx, nss())
            .thenRunOn(Grid::get(_opCtx)->getExecutorPool()->getFixedExecutor())
            .getAsync([](auto) {});
        uassertStatusOK(migrationCommitStatus);
    }

    // Asynchronously tell the recipient to release its critical section
    _coordinator->launchReleaseRecipientCriticalSection(_opCtx);

    hangBeforePostMigrationCommitRefresh.pauseWhileSet();

    try {
        LOGV2_DEBUG_OPTIONS(4817404,
                            2,
                            {logv2::LogComponent::kShardMigrationPerf},
                            "Starting post-migration commit refresh on the shard",
                            "migrationId"_attr = _coordinator->getMigrationId(),
                            logAttrs(nss()));

        FilteringMetadataCache::get(_opCtx)->forceCollectionPlacementRefresh(_opCtx, nss());
        FilteringMetadataCache::get(_opCtx)->waitForCollectionFlush(_opCtx, nss());

        LOGV2_DEBUG_OPTIONS(4817405,
                            2,
                            {logv2::LogComponent::kShardMigrationPerf},
                            "Finished post-migration commit refresh on the shard",
                            "migrationId"_attr = _coordinator->getMigrationId(),
                            logAttrs(nss()));
    } catch (const DBException& ex) {
        LOGV2_DEBUG_OPTIONS(4817410,
                            2,
                            {logv2::LogComponent::kShardMigrationPerf},
                            "Finished post-migration commit refresh on the shard with error",
                            "migrationId"_attr = _coordinator->getMigrationId(),
                            logAttrs(nss()),
                            "error"_attr = redact(ex));
        {
            // TODO (SERVER-71444): Fix to be interruptible or document exception.
            UninterruptibleLockGuard noInterrupt(_opCtx);  // NOLINT.
            AutoGetCollection autoColl(_opCtx, nss(), MODE_IX);
            CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(_opCtx, nss())
                ->clearFilteringMetadata(_opCtx);
        }
        scopedGuard.dismiss();
        _cleanup(false);
        throw;
    }

    // Migration succeeded

    const auto refreshedMetadata = _getCurrentMetadataAndCheckForConflictingErrors();
    {
        // Emit an oplog entry about the completion of the migration
        const bool noMoreCollectionChunksOnDonor =
            !refreshedMetadata.getChunkManager()->getVersion(_args.getFromShard()).isSet();
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
          "migrationId"_attr = _coordinator->getMigrationId(),
          logAttrs(nss()));

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
          "migrationId"_attr = _coordinator->getMigrationId(),
          logAttrs(nss()),
          "durationMillis"_attr = t.millis());

    // Exit the critical section and ensure that all the necessary state is fully persisted before
    // scheduling orphan cleanup.
    _cleanup(true);

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
              "migrationId"_attr = _coordinator->getMigrationId(),
              logAttrs(nss()),
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

void MigrationSourceManager::_cleanupOnError() {
    if (_state == kDone) {
        return;
    }

    ShardingLogging::get(_opCtx)->logChange(
        _opCtx,
        "moveChunk.error",
        _args.getCommandParameter(),
        BSON("min" << *_args.getMin() << "max" << *_args.getMax() << "from" << _args.getFromShard()
                   << "to" << _args.getToShard()),
        defaultMajorityWriteConcernDoNotUse());

    _cleanup(true);
}

SharedSemiFuture<void> MigrationSourceManager::abort() {
    stdx::lock_guard<Client> lk(*_opCtx->getClient());
    _opCtx->markKilled();
    _stats.countDonorMoveChunkAbortConflictingIndexOperation.addAndFetch(1);

    return _completion.getFuture();
}

CollectionMetadata MigrationSourceManager::_getCurrentMetadataAndCheckForConflictingErrors() {
    auto metadata = [&] {
        const auto scopedCsr =
            CollectionShardingRuntime::acquireShared(_opCtx, _args.getCommandParameter());

        const auto optMetadata = scopedCsr->getCurrentMetadataIfKnown();
        uassert(ErrorCodes::ConflictingOperationInProgress,
                "The collection's sharding state was cleared by a concurrent operation",
                optMetadata);
        return *optMetadata;
    }();
    uassert(ErrorCodes::ConflictingOperationInProgress,
            str::stream()
                << "The collection's timestamp has changed since the migration began. Expected "
                   "timestamp: "
                << _collectionTimestamp.toStringPretty() << ", but found: "
                << (metadata.isSharded()
                        ? metadata.getCollPlacementVersion().getTimestamp().toStringPretty()
                        : "unsharded collection"),
            metadata.isSharded() &&
                _collectionTimestamp == metadata.getCollPlacementVersion().getTimestamp());

    return metadata;
}

void MigrationSourceManager::_cleanup(bool completeMigration) {
    invariant(_state != kDone);

    auto cloneDriver = [&]() {
        // Unregister from the collection's sharding state.
        // TODO (SERVER-71444): Fix to be interruptible or document exception.
        UninterruptibleLockGuard noInterrupt(_opCtx);  // NOLINT.
        // Cleanup might be happening because the node is stepping down from primary, we don't want
        // to fail declaring write intent if that is the case.
        auto autoGetCollOptions =
            AutoGetCollection::Options{}.globalLockOptions(Lock::GlobalLockOptions{
                .explicitIntent = rss::consensus::IntentRegistry::Intent::LocalWrite});
        AutoGetCollection autoColl(_opCtx, nss(), MODE_IX, autoGetCollOptions);
        auto scopedCsr =
            CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(_opCtx, nss());

        if (_state != kCreated) {
            invariant(_cloneDriver);
        }
        return std::move(_cloneDriver);
    }();

    // Exit the migration critical section.
    _critSec.reset();

    if (_state == kCriticalSection || _state == kCloneCompleted || _state == kCommittingOnConfig) {
        LOGV2_DEBUG_OPTIONS(4817403,
                            2,
                            {logv2::LogComponent::kShardMigrationPerf},
                            "Finished critical section",
                            "migrationId"_attr = _coordinator->getMigrationId(),
                            logAttrs(nss()));

        LOGV2(6107802,
              "Finished critical section",
              "migrationId"_attr = _coordinator->getMigrationId(),
              logAttrs(nss()),
              "durationMillis"_attr = _cloneAndCommitTimer.millis());
    }

    // The cleanup operations below are potentially blocking or acquire other locks, so perform them
    // outside of the collection X lock

    if (cloneDriver) {
        cloneDriver->cancelClone(_opCtx);
    }

    try {
        if (_state >= kCloning) {
            invariant(_coordinator);
            if (_state < kCommittingOnConfig) {
                _coordinator->setMigrationDecision(DecisionEnum::kAborted);
            }

            auto newClient = _opCtx->getServiceContext()
                                 ->getService(ClusterRole::ShardServer)
                                 ->makeClient("MigrationCoordinator");
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
                _cleanupCompleteFuture = _coordinator->completeMigration(newOpCtx);
            }
        }

        _state = kDone;
    } catch (const DBException& ex) {
        LOGV2_WARNING(5089001,
                      "Failed to complete the migration",
                      "chunkMigrationRequestParameters"_attr = redact(_args.toBSON()),
                      "error"_attr = redact(ex),
                      "migrationId"_attr = _coordinator->getMigrationId(),
                      logAttrs(nss()));
        // Something went really wrong when completing the migration just unset the metadata and let
        // the next op to recover.
        // TODO (SERVER-71444): Fix to be interruptible or document exception.
        UninterruptibleLockGuard noInterrupt(_opCtx);  // NOLINT.
        auto autoGetCollOptions =
            AutoGetCollection::Options{}.globalLockOptions(Lock::GlobalLockOptions{
                .explicitIntent = rss::consensus::IntentRegistry::Intent::LocalWrite});
        AutoGetCollection autoColl(_opCtx, nss(), MODE_IX, autoGetCollOptions);
        CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(_opCtx, nss())
            ->clearFilteringMetadata(_opCtx);
    }
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
