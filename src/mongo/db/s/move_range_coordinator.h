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

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/global_catalog/ddl/chunk_operation_sharding_coordinator.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/active_migrations_registry.h"
#include "mongo/db/s/migration_source_manager.h"
#include "mongo/db/s/move_range_coordinator_document_gen.h"
#include "mongo/db/sharding_environment/sharding_statistics.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/logv2/attribute_storage.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <memory>

#include <boost/optional.hpp>

namespace mongo {

class [[MONGO_MOD_PUBLIC]] MoveRangeCoordinator final
    : public ChunkOperationShardingCoordinator<MoveRangeCoordinatorDocument> {
public:
    MoveRangeCoordinator(ShardingCoordinatorService* service, const BSONObj& initialStateDoc);

    ~MoveRangeCoordinator() override = default;

    void checkIfOptionsConflict(const BSONObj& doc) const final;

    void appendCommandInfo(BSONObjBuilder* cmdInfoBuilder) const override;

    logv2::DynamicAttributes getCoordinatorLogAttrs() const override;

protected:
    bool isInCriticalSection(Phase phase) const override;

    ChunkOperationsStatistics::ChunkOperationType chunkOperationMetricType() const override {
        return ChunkOperationsStatistics::ChunkOperationType::kMoveRange;
    }

private:
    bool _mustAlwaysMakeProgress() override {
        return true;
    }

    bool _shouldUseCancelableOpCtx() const override {
        return true;
    }

    ExecutorFuture<void> _acquireLocksAsync(OperationContext* opCtx,
                                            std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                            const CancellationToken& token) override;

    void _releaseLocks(OperationContext* opCtx) override;

    ExecutorFuture<void> _runImpl(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                  const CancellationToken& token) noexcept override;

    // Run by the framework when the coordinator aborts. Releases the donor critical section and
    // completes the migration as aborted from the persisted document, without the in-memory
    // migration attempt. Idempotent: the framework may retry it.
    ExecutorFuture<void> _cleanupOnAbort(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                         const CancellationToken& token,
                                         const Status& status) noexcept override;

    ExecutorFuture<void> _joinExistingExecution(
        std::shared_ptr<executor::ScopedTaskExecutor> executor);

    // Drives the kGlobalCatalogCommit phase: sends the idempotent commit command, built from
    // durable state so it can be re-sent after a failover. Returns the chunks changed by the commit
    // (in config BSON format) so the caller can persist them for the shard-catalog commit phase.
    std::vector<BSONObj> _commitToGlobalCatalog(OperationContext* opCtx);

    // Drives the kPostGlobalCatalogCommit phase: runs the post-commit bookkeeping after the
    // global-catalog commit (change-stream event, in-memory bookkeeping via the migration attempt
    // when present, and persisting the committed decision).
    void _postGlobalCatalogCommit(OperationContext* opCtx);

    // Sets the decision on the coordinator document and persists it durably.
    void _persistMigrationDecision(OperationContext* opCtx,
                                   MigrationCoordinatorDocument& doc,
                                   DecisionEnum decision);

    // Returns the persisted MigrationCoordinatorDocument for this migration, if present. Used by
    // the recovery finalization path to reconstruct the migration coordinator after a failover.
    boost::optional<MigrationCoordinatorDocument> _getMigrationCoordinatorDocumentIfExists(
        OperationContext* opCtx);

    // Releases the recipient critical section and completes the migration coordinator. Dispatches
    // to the live MigrationSourceManager on the commit term, or reconstructs the coordinator from
    // its persisted document on the recovery path.
    void _finalizeMigration(OperationContext* opCtx);

    // Final teardown shared by the success and abort paths. Releases the donor critical section,
    // completes the migration coordinator from its persisted decision (releasing the recipient
    // critical section, scheduling range deletion, forgetting the on-disk document), notifies the
    // range-deleter recovery job after failover, and unblocks joined migrations with `outcome`. The
    // caller must have persisted the migration decision (committed earlier, aborted just before
    // calling). Idempotent.
    void _releaseCriticalSectionAndFinalize(OperationContext* opCtx, const Status& outcome);

    // Commits the post-migration collection and chunk metadata to the authoritative local shard
    // catalog on the donor and recipient. Must run while the critical section is held.
    void _commitToShardCatalog(OperationContext* opCtx,
                               const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
                               const CancellationToken& token);

    class MigrationAttempt {
    public:
        MigrationAttempt(OperationContext* opCtx,
                         CancellationToken token,
                         NamespaceString nss,
                         ShardsvrMoveRangeRequest request,
                         WriteConcernOptions writeConcern,
                         UUID migrationId);

        Status migrate(OperationContext* opCtx);
        // Promotes the donor critical section to also block reads, just before the config commit.
        void promoteCriticalSection(OperationContext* opCtx);
        // The donor shard's placement version captured when the clone started. The coordinator
        // persists it so the global-catalog commit can be re-issued from durable state.
        ChunkVersion donorShardVersionPreMigration() const;
        // Marks that the global-catalog commit is about to be issued, so a teardown is treated as
        // uncertain rather than aborted.
        void markCommitInProgress();
        // Post-commit bookkeeping once the global-catalog commit has succeeded: clears the
        // time-series bucket catalog if needed, records the committed decision, and writes the
        // moveChunk.commit changelog entry.
        void recordCommitSuccess(OperationContext* opCtx);
        // Completes the migration coordinator on the same term that ran the commit (releasing the
        // recipient critical section). Honours waitForDelete.
        void finalize(OperationContext* opCtx);

    private:
        class CloneMetricsSnapshot {
        public:
            explicit CloneMetricsSnapshot(OperationContext* opCtx)
                : _docs(ShardingStatistics::get(opCtx).countDocsClonedOnDonor.load()),
                  _bytes(ShardingStatistics::get(opCtx).countBytesClonedOnDonor.load()),
                  _millis(ShardingStatistics::get(opCtx).totalDonorChunkCloneTimeMillis.load()) {}

            long long docsCloned(OperationContext* opCtx) const {
                return ShardingStatistics::get(opCtx).countDocsClonedOnDonor.load() - _docs;
            }
            long long bytesCloned(OperationContext* opCtx) const {
                return ShardingStatistics::get(opCtx).countBytesClonedOnDonor.load() - _bytes;
            }
            long long cloneTimeMillis(OperationContext* opCtx) const {
                return ShardingStatistics::get(opCtx).totalDonorChunkCloneTimeMillis.load() -
                    _millis;
            }

        private:
            const long long _docs;
            const long long _bytes;
            const long long _millis;
        };

        const NamespaceString _nss;
        const ShardsvrMoveRangeRequest _request;
        const WriteConcernOptions _writeConcern;
        const UUID _migrationId;
        ServiceContext::UniqueClient _ownedClient;
        CancelableOperationContext _ownedOpCtx;
        CloneMetricsSnapshot _cloneMetrics;
        std::unique_ptr<MigrationSourceManager> _msm;
        boost::optional<Status> _migrateResult;
    };

    const ShardsvrMoveRangeRequest _request;
    const std::shared_ptr<executor::TaskExecutor> _cleanupExecutor;
    boost::optional<MigrationAttempt> _migrationAttempt;
    boost::optional<ScopedDonateChunk> _scopedDonateChunk;
};

}  // namespace mongo
