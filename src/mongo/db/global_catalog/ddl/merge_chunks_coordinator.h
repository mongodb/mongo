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

#include "mongo/db/global_catalog/ddl/chunk_operation_sharding_coordinator.h"
#include "mongo/db/global_catalog/ddl/merge_chunks_coordinator_document_gen.h"
#include "mongo/db/s/active_migrations_registry.h"

namespace mongo {

class MergeChunksCoordinator final
    : public ChunkOperationShardingCoordinator<MergeChunksCoordinatorDocument> {
public:
    MergeChunksCoordinator(ShardingCoordinatorService* service, const BSONObj& initialStateDoc);

    void checkIfOptionsConflict(const BSONObj& doc) const final;

    void appendCommandInfo(BSONObjBuilder* cmdInfoBuilder) const override;

protected:
    bool isInCriticalSection(Phase phase) const override;

private:
    ExecutorFuture<void> _acquireLocksAsync(OperationContext* opCtx,
                                            std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                            const CancellationToken& token) override;

    void _releaseLocks(OperationContext* opCtx) override;

    ExecutorFuture<void> _runImpl(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                  const CancellationToken& token) noexcept override;

    const ShardsvrMergeChunksRequest _request;
    boost::optional<ScopedSplitMergeChunk> _scopedSplitMergeChunk;
};

}  // namespace mongo
