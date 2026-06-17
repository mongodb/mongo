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
#include "mongo/db/client.h"
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

ConnectionString makeDonorConnStr(OperationContext* opCtx,
                                  const ShardsvrMoveRangeRequest& request) {
    return uassertStatusOK(
               Grid::get(opCtx)->shardRegistry()->getShard(opCtx, request.getFromShard()))
        ->getConnString();
}

HostAndPort makeRecipientHost(OperationContext* opCtx, const ShardsvrMoveRangeRequest& request) {
    auto recipientShard =
        uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, request.getToShard()));
    return uassertStatusOK(recipientShard->getTargeter()->findHost(
        opCtx, ReadPreferenceSetting{ReadPreference::PrimaryOnly}, {}));
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

ExecutorFuture<void> MoveRangeCoordinator::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    if (!_scopedDonateChunk->mustExecute()) {
        // MoveRangeCoordinators will automatically join (if they have the same parameters) or
        // serialize (otherwise) with other MoveRangeCoordinators. The only reason we'd need to join
        // here is if there is an ongoing migration using the legacy path.
        // TODO SERVER-127253: Remove this when the legacy path is removed.
        return _joinExistingExecution(executor);
    }
    return ExecutorFuture<void>(**executor)
        .then(
            _buildPhaseHandler(Phase::kMigrate,
                               [this, anchor = shared_from_this(), token](OperationContext* opCtx) {
                                   LOGV2(12894207,
                                         "MoveRangeCoordinator executing kMigrate",
                                         logAttrs(nss()),
                                         "migrationId"_attr = _doc.getMigrationId());
                                   uassert(ErrorCodes::InterruptedDueToReplStateChange,
                                           "MoveRangeCoordinator interrupted during data transfer",
                                           !_recoveredFromDisk);
                                   if (!_migrationAttempt) {
                                       _migrationAttempt.emplace(
                                           opCtx,
                                           token,
                                           nss(),
                                           _request,
                                           _doc.getWriteConcern().value_or(WriteConcernOptions{}),
                                           _doc.getMigrationId());
                                   }
                                   uassertStatusOK(_migrationAttempt->migrate(opCtx));
                               }))
        .then(_buildPhaseHandler(Phase::kCommit,
                                 [this, anchor = shared_from_this()](OperationContext* opCtx) {
                                     LOGV2(12894208,
                                           "MoveRangeCoordinator executing kCommit",
                                           logAttrs(nss()),
                                           "migrationId"_attr = _doc.getMigrationId());
                                     uassert(ErrorCodes::InterruptedDueToReplStateChange,
                                             "MoveRangeCoordinator interrupted during commit",
                                             !_recoveredFromDisk);
                                     tassert(
                                         12894200,
                                         "Migrate and commit must only run during the same term",
                                         _migrationAttempt.has_value());
                                     _writeCommitAttempted(opCtx);
                                     uassertStatusOK(_migrationAttempt->commit(opCtx));
                                 }))
        .onCompletion([this, anchor = shared_from_this()](Status migrateStatus) {
            auto opCtx = makeOperationContext(/*deprioritizable=*/false);
            if (!migrateStatus.isOK()) {
                // OrphanedRangeCleanUpFailed is only thrown if the migration succeeded, but
                // _waitForDelete was requested and we failed to delete all orphans (e.g. because we
                // were interrupted before the range deletion could complete). Arguably, we should
                // not return success for a _waitForDelete request if we failed to delete, but
                // legacy semantics are to return OK for any migration that succeeded to commit,
                // regardless of the outcome of _waitForDelete.
                if (migrateStatus.code() == ErrorCodes::OrphanedRangeCleanUpFailed) {
                    migrateStatus = Status::OK();
                } else {
                    LOGV2_WARNING(12697303,
                                  "Error while doing moveChunk",
                                  logAttrs(nss()),
                                  "error"_attr = redact(migrateStatus),
                                  "errorCode"_attr = migrateStatus.codeString());
                    if (migrateStatus.code() == ErrorCodes::LockTimeout) {
                        ShardingStatistics::get(opCtx.get())
                            .countDonorMoveChunkLockTimeout.addAndFetch(1);
                    }
                }
            }
            _writeMigrationResult(opCtx.get(), migrateStatus);
        })
        .then(_buildPhaseHandler(
            Phase::kEnsureMigrationCoordinatorComplete,
            [this, anchor = shared_from_this(), executor, token](OperationContext* opCtx) {
                LOGV2(12894209,
                      "MoveRangeCoordinator executing kEnsureMigrationCoordinatorComplete",
                      logAttrs(nss()),
                      "migrationId"_attr = _doc.getMigrationId(),
                      "commitAttempted"_attr = _doc.getCommitAttempted());
                if (!_migrationCoordinatorDocumentMayExist(opCtx)) {
                    return;
                }
                LOGV2(12894205,
                      "MoveRangeCoordinator recovering in-progress migration",
                      logAttrs(nss()),
                      "migrationId"_attr = _doc.getMigrationId(),
                      "commitAttempted"_attr = _doc.getCommitAttempted());
                const auto outcome = _recoveryFlow(executor, token).get(opCtx);
                if (outcome == MigrationOutcome::kCommitted) {
                    _overwriteMigrationResultWithOk(opCtx);
                } else {
                    auto result = _getMigrationResult();
                    tassert(12894201,
                            "Migration result must be an error after abort",
                            result && !result->isOK());
                }
            }))
        .then(_buildPhaseHandler(
            Phase::kCompleteMigration,
            [this, anchor = shared_from_this(), executor](OperationContext* opCtx) {
                LOGV2(12894210,
                      "MoveRangeCoordinator executing kCompleteMigration",
                      logAttrs(nss()),
                      "migrationId"_attr = _doc.getMigrationId());
                tassert(12894202,
                        "migrationResult must be set before kCompleteMigration",
                        _getMigrationResult().has_value());
                const auto completionStatus = *_getMigrationResult();

                PersistentTaskStore<MigrationCoordinatorDocument> store(
                    NamespaceString::kMigrationCoordinatorsNamespace);
                tassert(
                    12723002,
                    "Expected no MigrationCoordinator document on disk before completing migration",
                    store.count(opCtx,
                                BSON(MigrationCoordinatorDocument::kIdFieldName
                                     << _doc.getMigrationId())) == 0);
                if (_recoveredFromDisk) {
                    const auto term = repl::ReplicationCoordinator::get(opCtx)->getTerm();
                    RangeDeleterService::get(opCtx)->notifyRecoveryJobComplete(term);
                }
                _scopedDonateChunk->signalComplete(completionStatus);
                _completeOnError = true;
                uassertStatusOK(completionStatus);
            }));
}

MoveRangeCoordinator::MigrationAttempt::MigrationAttempt(OperationContext* opCtx,
                                                         CancellationToken token,
                                                         NamespaceString nss,
                                                         ShardsvrMoveRangeRequest request,
                                                         WriteConcernOptions writeConcern,
                                                         UUID migrationId)
    : _nss(std::move(nss)),
      _request(std::move(request)),
      _writeConcern(std::move(writeConcern)),
      _migrationId(std::move(migrationId)),
      _ownedClient(opCtx->getService()->makeClient("MoveRangeCoordinator::MigrationAttempt")),
      _ownedOpCtx(_ownedClient->makeOperationContext(),
                  token,
                  Grid::get(opCtx)->getExecutorPool()->getFixedExecutor()),
      _cloneMetrics(opCtx) {
    AlternativeClientRegion acr(_ownedClient);
    _msm = MigrationSourceManager::createMigrationSourceManager(
        _ownedOpCtx.get(),
        buildShardsvrMoveRange(_nss, _request),
        WriteConcernOptions{_writeConcern},
        makeDonorConnStr(_ownedOpCtx.get(), _request),
        makeRecipientHost(_ownedOpCtx.get(), _request),
        ManagementModeEnum::kMoveRangeCoordinator,
        _migrationId);
}

Status MoveRangeCoordinator::MigrationAttempt::migrate(OperationContext* opCtx) {
    if (_migrateResult)
        return *_migrateResult;

    AlternativeClientRegion acr(_ownedClient);
    try {
        _msm->startClone();
        _msm->awaitToCatchUp();
        _msm->enterCriticalSection();
        _msm->commitChunkOnRecipient();
        _migrateResult = Status::OK();
    } catch (const DBException& ex) {
        _migrateResult = ex.toStatus();
    }

    return *_migrateResult;
}

Status MoveRangeCoordinator::MigrationAttempt::commit(OperationContext* opCtx) {
    tassert(12894203, "commit() called before migrate() completed", _migrateResult.has_value());
    if (!_migrateResult->isOK())
        return *_migrateResult;

    if (_commitResult)
        return *_commitResult;

    AlternativeClientRegion acr(_ownedClient);
    try {
        // Any failure after this point may have committed on the config server, so
        // the recovery flow is required to determine the actual outcome.
        _msm->commitChunkMetadataOnConfig();

        LOGV2(12697302,
              "Migration finished",
              "migrationId"_attr = _migrationId.toString(),
              "totalTimeMillis"_attr = _msm->getOpTimeMillis(),
              "docsCloned"_attr = _cloneMetrics.docsCloned(opCtx),
              "bytesCloned"_attr = _cloneMetrics.bytesCloned(opCtx),
              "cloneTime"_attr = _cloneMetrics.cloneTimeMillis(opCtx));

        _commitResult = Status::OK();
    } catch (const DBException& ex) {
        _commitResult = ex.toStatus();
    }

    return *_commitResult;
}

boost::optional<Status> MoveRangeCoordinator::_getMigrationResult() const {
    if (_doc.getMigrationSuccess().value_or(false))
        return Status::OK();
    if (const auto& failure = _doc.getMigrationFailure())
        return *failure;
    return boost::none;
}

void MoveRangeCoordinator::_writeMigrationResult(OperationContext* opCtx, Status result) {
    if (_doc.getMigrationSuccess().value_or(false) || _doc.getMigrationFailure()) {
        return;
    }
    auto newDoc = MoveRangeCoordinatorDocument(_doc);
    if (result.isOK()) {
        newDoc.setMigrationSuccess(true);
    } else {
        newDoc.setMigrationFailure(result);
    }
    _updateStateDocument(opCtx, std::move(newDoc));
}

void MoveRangeCoordinator::_writeCommitAttempted(OperationContext* opCtx) {
    auto newDoc = MoveRangeCoordinatorDocument(_doc);
    newDoc.setCommitAttempted(true);
    _updateStateDocument(opCtx, std::move(newDoc));
}

void MoveRangeCoordinator::_overwriteMigrationResultWithOk(OperationContext* opCtx) {
    tassert(12894204,
            "Cannot overwrite a migration result with success if no commit was attempted",
            _doc.getCommitAttempted().value_or(false));
    auto newDoc = MoveRangeCoordinatorDocument(_doc);
    newDoc.setMigrationSuccess(true);
    newDoc.setMigrationFailure(boost::none);
    _updateStateDocument(opCtx, std::move(newDoc));
}

ExecutorFuture<MoveRangeCoordinator::MigrationOutcome> MoveRangeCoordinator::_recoveryFlow(
    std::shared_ptr<executor::ScopedTaskExecutor> executor, const CancellationToken& token) {
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
                .until([this](Status status) {
                    if (!status.isOK()) {
                        LOGV2_WARNING(12894206,
                                      "MoveRangeCoordinator migration recovery retrying",
                                      logAttrs(nss()),
                                      "migrationId"_attr = _doc.getMigrationId(),
                                      "error"_attr = redact(status));
                    }
                    return status.isOK();
                })
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
        .then([this, anchor = shared_from_this()] {
            auto opCtx = makeOperationContext(/*deprioritizable=*/true);

            // Determine whether the migration committed or aborted by checking if the migrated
            // range's min key still belongs to this shard. If it does, the chunk never moved
            // (migration aborted). This mirrors the decision logic in
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

            return migrationAborted ? MigrationOutcome::kAborted : MigrationOutcome::kCommitted;
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


}  // namespace mongo
