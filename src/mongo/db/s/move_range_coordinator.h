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
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <memory>

#include <boost/optional.hpp>

namespace mongo {

class MONGO_MOD_PUBLIC MoveRangeCoordinator final
    : public ChunkOperationShardingCoordinator<MoveRangeCoordinatorDocument> {
public:
    MoveRangeCoordinator(ShardingCoordinatorService* service, const BSONObj& initialStateDoc);

    ~MoveRangeCoordinator() override = default;

    void checkIfOptionsConflict(const BSONObj& doc) const final;

    void appendCommandInfo(BSONObjBuilder* cmdInfoBuilder) const override;

protected:
    bool isInCriticalSection(Phase phase) const override;

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

    ExecutorFuture<void> _joinExistingExecution(
        std::shared_ptr<executor::ScopedTaskExecutor> executor);

    enum class MigrationOutcome { kCommitted, kAborted };

    ExecutorFuture<MigrationOutcome> _recoveryFlow(
        std::shared_ptr<executor::ScopedTaskExecutor> executor, const CancellationToken& token);

    bool _migrationCoordinatorDocumentMayExist(OperationContext* opCtx);

    boost::optional<Status> _getMigrationResult() const;
    void _writeMigrationResult(OperationContext* opCtx, Status result);
    void _overwriteMigrationResultWithOk(OperationContext* opCtx);
    void _writeCommitAttempted(OperationContext* opCtx);

    class MigrationAttempt {
    public:
        MigrationAttempt(OperationContext* opCtx,
                         CancellationToken token,
                         NamespaceString nss,
                         ShardsvrMoveRangeRequest request,
                         WriteConcernOptions writeConcern,
                         UUID migrationId);

        Status migrate(OperationContext* opCtx);
        Status commit(OperationContext* opCtx);

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
        boost::optional<Status> _commitResult;
    };

    const ShardsvrMoveRangeRequest _request;
    const std::shared_ptr<executor::TaskExecutor> _cleanupExecutor;
    boost::optional<MigrationAttempt> _migrationAttempt;
    boost::optional<ScopedDonateChunk> _scopedDonateChunk;
};

}  // namespace mongo
