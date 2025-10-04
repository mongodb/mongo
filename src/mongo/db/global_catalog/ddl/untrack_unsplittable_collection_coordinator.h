/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator_service.h"
#include "mongo/db/global_catalog/ddl/untrack_unsplittable_collection_coordinator_document_gen.h"

namespace mongo {

class UntrackUnsplittableCollectionCoordinator final
    : public RecoverableShardingDDLCoordinator<UntrackUnsplittableCollectionCoordinatorDocument,
                                               UntrackUnsplittableCollectionCoordinatorPhaseEnum> {
public:
    using CoordDoc = UntrackUnsplittableCollectionCoordinatorDocument;
    using Phase = UntrackUnsplittableCollectionCoordinatorPhaseEnum;
    UntrackUnsplittableCollectionCoordinator(ShardingDDLCoordinatorService* service,
                                             const BSONObj& initialState)
        : RecoverableShardingDDLCoordinator(
              service, "UntrackUnsplittableCollectionCoordinator", initialState),
          _critSecReason(BSON("command"
                              << "untrackCollection"
                              << "ns"
                              << NamespaceStringUtil::serialize(
                                     originalNss(), SerializationContext::stateDefault()))) {}

    ~UntrackUnsplittableCollectionCoordinator() override = default;

    void checkIfOptionsConflict(const BSONObj& coorDoc) const override;

    void appendCommandInfo(BSONObjBuilder* cmdInfoBuilder) const override;

private:
    const BSONObj _critSecReason;

    bool _mustAlwaysMakeProgress() override {
        return _doc.getPhase() > Phase::kUnset;
    }

    StringData serializePhase(const Phase& phase) const override {
        return UntrackUnsplittableCollectionCoordinatorPhase_serializer(phase);
    }

    void _checkPreconditions();

    void _enterCriticalSection(OperationContext* opCtx,
                               std::shared_ptr<executor::ScopedTaskExecutor> executor,
                               const CancellationToken& token);

    void _commitUntrackCollection(OperationContext* opCtx,
                                  std::shared_ptr<executor::ScopedTaskExecutor> executor);

    void _exitCriticalSection(OperationContext* opCtx,
                              std::shared_ptr<executor::ScopedTaskExecutor> executor,
                              const CancellationToken& token);

    void _fireAndForgetRefresh();

    ExecutorFuture<void> _runImpl(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                  const CancellationToken& token) noexcept override;
};
}  // namespace mongo
