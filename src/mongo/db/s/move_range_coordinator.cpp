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
#include "mongo/db/s/active_migrations_registry.h"
#include "mongo/db/s/migration_source_manager.h"
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

ExecutorFuture<void> MoveRangeCoordinator::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    return ExecutorFuture<void>(**executor)
        .then([this, anchor = shared_from_this()] {
            auto opCtxHolder = makeOperationContext(/*deprioritizable=*/true);
            auto* opCtx = opCtxHolder.get();

            // Preserve the interruption semantics of the legacy command handler: MSM's blocking
            // phases (notably enterCriticalSection, which can wait up to 6h) must be interrupted
            // on primary stepdown via the opCtx so the critical section is released promptly.
            // makeOperationContext() does NOT set this; we must opt in explicitly.
            // TODO: Plumb the coordinator's CancellationToken into MSM so non-stepdown
            // cancellations (e.g. coordinator service shutdown) also interrupt the migration.
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();


            // Short-circuit a same-shard move before any I/O. Matches the legacy command's
            // behavior; no ActiveMigrationsRegistry slot is needed for a same-shard move.
            if (_request.getFromShard() == _request.getToShard()) {
                return;
            }

            // TODO (SERVER-127230): Move acquisition and release of the
            // ActiveMigrationsRegistry into _acquireLocksAsync() and _releaseLocks() once
            // failover recovery is handled by the ShardingCoordinatorService.
            //
            // Until then, releasing the ActiveMigrationsRegistry from _releaseLocks() can
            // deadlock during step-up with resumeMigrationCoordinationsOnStepUp(). This is
            // because stepdown does not call _releaseLocks(); instead, the locks are
            // released during step-up, after resumeMigrationCoordinationsOnStepUp() has
            // already been invoked.
            auto scopedDonateChunk = uassertStatusOK(
                ActiveMigrationsRegistry::get(opCtx).registerDonateChunk(opCtx, nss(), _request));

            // The coordinator service guarantees at most one MoveRangeCoordinator instance
            // per namespace. A join path (mustExecute == false) on this code path could only
            // mean a legacy ScopedDonateChunk migration is still running for the same
            // namespace — assert so the framework's outer AsyncTry retries with exponential
            // backoff until the legacy migration releases the registry.
            uassert(12697301,
                    "Joined an existing ScopedDonateChunk while running as a "
                    "MoveRangeCoordinator; retrying lock acquisition",
                    scopedDonateChunk.mustExecute());

            Status migrationStatus{ErrorCodes::InternalError,
                                   "MoveRangeCoordinator did not complete"};
            try {
                const auto writeConcern = _doc.getWriteConcern().value_or(WriteConcernOptions{});

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
                        recipientHost);

                migrationSourceManager.startClone();
                migrationSourceManager.awaitToCatchUp();
                migrationSourceManager.enterCriticalSection();
                migrationSourceManager.commitChunkOnRecipient();
                migrationSourceManager.commitChunkMetadataOnConfig();

                long long docsCloned =
                    ShardingStatistics::get(opCtx).countDocsClonedOnDonor.load() - totalDocsCloned;
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
                    ShardingStatistics::get(opCtx).countDonorMoveChunkLockTimeout.addAndFetch(1);
                }
            }

            // signalComplete() satisfies ScopedDonateChunk's destructor invariant; the
            // destructor (running on stack unwind below) is what actually clears the
            // registry slot via _clearDonateChunk().
            scopedDonateChunk.signalComplete(migrationStatus);

            // Re-throw on error so the outer .onError/.onCompletion observe the failure
            // status. On stepdown the framework wrappers will short-circuit those user
            // bodies (executor is shut down), but that's fine — the slot is already being
            // released by the local going out of scope right after this throw.
            uassertStatusOK(migrationStatus);
        })
        .onError([this, anchor = shared_from_this()](Status status) {
            // Mimic the legacy _shardsvrMoveRange command: every error surfaced by
            // MigrationSourceManager is terminal for this migration attempt; the policy about
            // whether to re-issue belongs to the caller (the balancer or a user-issued
            // sh.moveRange), not to the coordinator instance.
            //
            // The framework's until-predicate consults _completeOnError ahead of
            // _isRetriableErrorForDDLCoordinator, so setting the flag here short-circuits the retry
            // loop on every kind of MSM failure — including categories the base classifier treats
            // as retriable (Interruption from index / collMod / killOp / LockTimeout,
            // WriteConcernError from unsatisfiable WC, CursorInvalidatedError that
            // MigrationChunkClonerSource::commitToConfig uses as the wire-format catch-all for
            // recipient-side data-transfer failures).
            _completeOnError = true;
            return status;
        });
}

}  // namespace mongo
