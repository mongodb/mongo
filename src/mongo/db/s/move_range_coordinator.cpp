/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/s/move_range_coordinator.h"

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/migration_coordinator_document_gen.h"
#include "mongo/db/s/migration_source_manager.h"
#include "mongo/db/s/range_deleter_service.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_runtime.h"
#include "mongo/db/shard_role/shard_catalog/shard_filtering_metadata_refresh.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/sharding_statistics.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/logv2/log.h"
#include "mongo/util/future_util.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

namespace {

// Rebuild the wire-format ShardsvrMoveRange command from the request struct stored on the
// coordinator. Required because MigrationSourceManager::createMigrationSourceManager still takes a
// full ShardsvrMoveRange and serializes it for its "Starting chunk migration donation" log.
ShardsvrMoveRange buildShardsvrMoveRange(const NamespaceString& nss,
                                         const ShardsvrMoveRangeRequest& request) {
    ShardsvrMoveRange cmd{nss};
    cmd.setDbName(nss.dbName());
    cmd.setShardsvrMoveRangeRequest(request);
    return cmd;
}

}  // namespace

MoveRangeCoordinator::MoveRangeCoordinator(ShardingCoordinatorService* service,
                                           const BSONObj& initialStateDoc)
    : ChunkOperationShardingCoordinator(service, "MoveRangeCoordinator", initialStateDoc),
      _request(_doc.getShardsvrMoveRangeRequest()) {}

void MoveRangeCoordinator::checkIfOptionsConflict(const BSONObj& doc) const {
    const auto otherDoc =
        MoveRangeCoordinatorDocument::parse(doc, IDLParserContext("MoveRangeCoordinatorDocument"));

    const auto& selfReq = _request.toBSON();
    const auto& otherReq = otherDoc.getShardsvrMoveRangeRequest().toBSON();

    uassert(ErrorCodes::ConflictingOperationInProgress,
            str::stream() << "Another move range operation for namespace "
                          << nss().toStringForErrorMsg()
                          << " is being executed with different parameters: " << redact(selfReq)
                          << " vs " << redact(otherReq),
            SimpleBSONObjComparator::kInstance.evaluate(selfReq == otherReq));
}

void MoveRangeCoordinator::appendCommandInfo(BSONObjBuilder* cmdInfoBuilder) const {
    cmdInfoBuilder->appendElements(_request.toBSON());
}

bool MoveRangeCoordinator::isInCriticalSection(Phase phase) const {
    return false;
}

ExecutorFuture<void> MoveRangeCoordinator::_acquireLocksAsync(
    OperationContext*,
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken&) {
    return ExecutorFuture<void>(**executor).then([this, anchor = shared_from_this()] {
        auto opCtx = makeOperationContext(/*deprioritizable=*/false);
        // registerDonateChunk has a legacy invariant requiring this flag. The cancellation
        // token already ensures this opCtx is interrupted on stepdown, but the invariant
        // checks for the flag specifically.
        opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();
        // Bypass the registry's waitForRecovery() when called from the coordinator recovery
        // path (_recoveredFromDisk == true): _acquireLocksAsync runs before
        // _constructionCompletionPromise is set, so waiting for recovery would deadlock against
        // the ShardingCoordinatorService waiting for this coordinator to finish construction.
        auto bypass = _recoveredFromDisk
            ? boost::optional<ActiveMigrationsRegistry::BypassRecoveryWait>(
                  makeRegistryRecoveryBypass())
            : boost::none;
        _scopedDonateChunk =
            uassertStatusOK(ActiveMigrationsRegistry::get(opCtx.get())
                                .registerDonateChunk(opCtx.get(), nss(), _request, bypass));
        if (_recoveredFromDisk) {
            ShardingStatistics::get(opCtx.get())
                .unfinishedMigrationFromPreviousPrimary.fetchAndAdd(1);
        }
    });
}

void MoveRangeCoordinator::_releaseLocks(OperationContext*) {
    _scopedDonateChunk.reset();
}

ExecutorFuture<void> MoveRangeCoordinator::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    if (!_recoveredFromDisk) {
        return _initialExecutionFlow(executor, token);
    }
    return _recoveryFlow(
        executor,
        token,
        Status{ErrorCodes::InterruptedDueToReplStateChange, "Migration aborted during recovery"});
}

ExecutorFuture<void> MoveRangeCoordinator::_joinExistingExecution(
    std::shared_ptr<executor::ScopedTaskExecutor> executor) {
    // A join token (mustExecute == false) means a legacy ScopedDonateChunk migration is still
    // in-flight for this namespace with identical parameters. Wait for it and propagate its result.
    return ExecutorFuture<void>(**executor).then([this, anchor = shared_from_this()] {
        auto opCtx = makeOperationContext(/*deprioritizable=*/false);
        _completeOnError = true;
        uassertStatusOK(_scopedDonateChunk->waitForCompletion(opCtx.get()));
    });
}

ExecutorFuture<void> MoveRangeCoordinator::_initialExecutionFlow(
    std::shared_ptr<executor::ScopedTaskExecutor> executor, const CancellationToken& token) {
    if (!_scopedDonateChunk->mustExecute()) {
        return _joinExistingExecution(executor);
    }
    return ExecutorFuture<void>(**executor)
        .then(_buildPhaseHandler(
            Phase::kMigrate,
            [this, anchor = shared_from_this(), executor](OperationContext* opCtx) {
                Status migrationStatus{ErrorCodes::InternalError,
                                       "MoveRangeCoordinator did not complete"};
                try {
                    const auto writeConcern =
                        _doc.getWriteConcern().value_or(WriteConcernOptions{});

                    // Resolve donor and recipient at execution time, not at coordinator-
                    // construction time: the recipient primary may change between command
                    // submission and the coordinator running on the fixed executor.
                    auto const shardRegistry = Grid::get(opCtx)->shardRegistry();

                    const auto donorConnStr =
                        uassertStatusOK(shardRegistry->getShard(opCtx, _request.getFromShard()))
                            ->getConnString();
                    const auto recipientHost = uassertStatusOK([&] {
                        auto recipientShard =
                            uassertStatusOK(shardRegistry->getShard(opCtx, _request.getToShard()));

                        return recipientShard->getTargeter()->findHost(
                            opCtx, ReadPreferenceSetting{ReadPreference::PrimaryOnly}, {});
                    }());

                    long long totalDocsCloned =
                        ShardingStatistics::get(opCtx).countDocsClonedOnDonor.load();
                    long long totalBytesCloned =
                        ShardingStatistics::get(opCtx).countBytesClonedOnDonor.load();
                    long long totalCloneTime =
                        ShardingStatistics::get(opCtx).totalDonorChunkCloneTimeMillis.load();

                    auto&& migrationSourceManager =
                        MigrationSourceManager::createMigrationSourceManager(
                            opCtx,
                            buildShardsvrMoveRange(nss(), _request),
                            WriteConcernOptions{writeConcern},
                            donorConnStr,
                            recipientHost,
                            ManagementModeEnum::kMoveRangeCoordinator,
                            _doc.getMigrationId());

                    migrationSourceManager.startClone();
                    migrationSourceManager.awaitToCatchUp();
                    migrationSourceManager.enterCriticalSection();
                    migrationSourceManager.commitChunkOnRecipient();
                    // Any failure after this point may have committed on the config server, so
                    // the recovery flow is required to determine the outcome.
                    _commitAttempted = true;
                    migrationSourceManager.commitChunkMetadataOnConfig();

                    long long docsCloned =
                        ShardingStatistics::get(opCtx).countDocsClonedOnDonor.load() -
                        totalDocsCloned;
                    long long bytesCloned =
                        ShardingStatistics::get(opCtx).countBytesClonedOnDonor.load() -
                        totalBytesCloned;
                    long long cloneTime =
                        ShardingStatistics::get(opCtx).totalDonorChunkCloneTimeMillis.load() -
                        totalCloneTime;
                    auto migrationId = migrationSourceManager.getMigrationId();

                    LOGV2(12697302,
                          "Migration finished",
                          "migrationId"_attr = migrationId ? migrationId->toString() : "",
                          "totalTimeMillis"_attr = migrationSourceManager.getOpTimeMillis(),
                          "docsCloned"_attr = docsCloned,
                          "bytesCloned"_attr = bytesCloned,
                          "cloneTime"_attr = cloneTime);

                    migrationStatus = Status::OK();
                } catch (const DBException& ex) {
                    migrationStatus = ex.toStatus();
                    LOGV2_WARNING(12697303,
                                  "Error while doing moveChunk",
                                  logAttrs(nss()),
                                  "error"_attr = redact(migrationStatus),
                                  "errorCode"_attr = migrationStatus.codeString());
                    if (migrationStatus.code() == ErrorCodes::LockTimeout) {
                        ShardingStatistics::get(opCtx).countDonorMoveChunkLockTimeout.addAndFetch(
                            1);
                    }
                }

                uassertStatusOK(migrationStatus);
                return _completeMigration(opCtx, executor, Status::OK());
            }))
        .onError([this, anchor = shared_from_this(), executor, token](Status status) {
            auto opCtx = makeOperationContext(/*deprioritizable=*/false);
            if (!_mustInitiateRecovery(opCtx.get())) {
                // The commit was never attempted and the MSM's cleanup path successfully removed
                // the MigrationCoordinator document.
                return _completeMigration(opCtx.get(), executor, status);
            }
            // Either the commit was attempted, or the MigrationCoordinator document is still on
            // disk (or we couldn't confirm it's gone). Enter the recovery flow to determine the
            // outcome.
            return _recoveryFlow(executor, token, status);
        });
}

ExecutorFuture<void> MoveRangeCoordinator::_recoveryFlow(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token,
    Status incomingStatus) {
    return ExecutorFuture<void>(**executor)
        .then([this, anchor = shared_from_this()] {
            tassert(12723001,
                    "Recovered MoveRangeCoordinator must be the migration executor, not a joiner",
                    _scopedDonateChunk->mustExecute());
            auto opCtx = makeOperationContext(/*deprioritizable=*/true);

            // Clearing the metadata forces _recoverMigrationCoordinations to run during the
            // refresh below (via the non-authoritative CSR state), which drives completeMigration
            // on the migrationCoordinators document and installs fresh filtering metadata.
            auto scopedCsr = CollectionShardingRuntime::acquireExclusive(opCtx.get(), nss());
            scopedCsr->clearCollectionMetadata(opCtx.get());
            // TODO (SERVER-127444): Remove this and tassert with the feature flag.
            scopedCsr->setNonAuthoritative();
        })
        .then([this, anchor = shared_from_this(), executor, token] {
            // migrationutil::refreshFilteringMetadataUntilSuccess doesn't play nicely with
            // stepdowns when invoked from a PrimaryOnlyService, so implement the loop directly.
            // This code is expected to go away after we enable authoritative commit anyway.
            return AsyncTry([this, anchor = shared_from_this()] {
                       auto opCtx = makeOperationContext(/*deprioritizable=*/true);
                       try {
                           uassertStatusOK(
                               FilteringMetadataCache::get(opCtx.get())
                                   ->onShardVersionMismatch(opCtx.get(), nss(), boost::none));
                       } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
                           // Can throw NamespaceNotFound if the collection/database was dropped.
                       }
                   })
                .until([](Status status) { return status.isOK(); })
                .withBackoffBetweenIterations(Backoff(Seconds(1), Milliseconds::max()))
                .on(**executor, token);
        })
        .then([this, anchor = shared_from_this()] {
            // Verify that _recoverMigrationCoordinations cleaned up the document as expected.
            // If it didn't (e.g. document deletion failed silently), throw so the outer
            // ShardingCoordinator retries the entire recovery flow from the beginning.
            auto opCtx = makeOperationContext(/*deprioritizable=*/true);
            uassert(12723003,
                    "MigrationCoordinator document still on disk after filtering metadata "
                    "refresh; retrying recovery",
                    !_migrationCoordinatorDocumentMayExist(opCtx.get()));
        })
        .then([this,
               anchor = shared_from_this(),
               executor,
               incomingStatus = std::move(incomingStatus)] {
            auto opCtx = makeOperationContext(/*deprioritizable=*/true);

            // Determine whether the migration committed or aborted by checking if the migrated
            // range's min key still belongs to this shard. If it does, the chunk never moved
            // (migration was aborted). This mirrors the decision logic in
            // _recoverMigrationCoordinations.
            //
            // getMin() is always set by the time the coordinator runs: the moveChunk/moveRange
            // command resolves any find-key to an explicit min/max before sending
            // _shardsvrMoveRange to the shard.
            const auto& min = _request.getMoveRangeRequestBase().getMin().value();
            const bool migrationAborted = [&] {
                auto scopedCsr = CollectionShardingRuntime::acquireShared(opCtx.get(), nss());
                const auto optMeta = scopedCsr->getCurrentMetadataIfKnown();
                return optMeta && optMeta->isSharded() && optMeta->keyBelongsToMe(min);
            }();

            // Notify the range deleter that recovery is complete. Only meaningful when
            // recovering after a failover.
            if (_recoveredFromDisk) {
                const auto term = repl::ReplicationCoordinator::get(opCtx.get())->getTerm();
                RangeDeleterService::get(opCtx.get())->notifyRecoveryJobComplete(term);
            }

            return _completeMigration(
                opCtx.get(), executor, migrationAborted ? incomingStatus : Status::OK());
        });
}

bool MoveRangeCoordinator::_migrationCoordinatorDocumentMayExist(OperationContext* opCtx) {
    // Treat any read failure as "document may exist": if we can't confirm it's gone, assume the
    // worst so that callers can trigger the recovery path.
    try {
        PersistentTaskStore<MigrationCoordinatorDocument> store(
            NamespaceString::kMigrationCoordinatorsNamespace);
        return store.count(
                   opCtx,
                   BSON(MigrationCoordinatorDocument::kIdFieldName << _doc.getMigrationId())) > 0;
    } catch (const DBException&) {
        return true;
    }
}

bool MoveRangeCoordinator::_mustInitiateRecovery(OperationContext* opCtx) {
    return _commitAttempted || _migrationCoordinatorDocumentMayExist(opCtx);
}

ExecutorFuture<void> MoveRangeCoordinator::_completeMigration(
    OperationContext* opCtx,
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    Status completionStatus) {
    PersistentTaskStore<MigrationCoordinatorDocument> store(
        NamespaceString::kMigrationCoordinatorsNamespace);
    tassert(12723002,
            "Expected no MigrationCoordinator document on disk before completing migration",
            store.count(
                opCtx, BSON(MigrationCoordinatorDocument::kIdFieldName << _doc.getMigrationId())) ==
                0);
    _scopedDonateChunk->signalComplete(completionStatus);
    _completeOnError = true;
    return ExecutorFuture<void>(**executor, completionStatus);
}

}  // namespace mongo
