/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/s/dist_lock_manager.h"
#include "mongo/db/s/forwardable_operation_metadata.h"
#include "mongo/db/s/sharding_ddl_coordinator_gen.h"
#include "mongo/executor/task_executor.h"
#include "mongo/util/future.h"

namespace mongo {

ShardingDDLCoordinatorMetadata extractShardingDDLCoordinatorMetadata(const BSONObj& coorDoc);

class ShardingDDLCoordinator
    : public repl::PrimaryOnlyService::TypedInstance<ShardingDDLCoordinator> {
public:
    explicit ShardingDDLCoordinator(const BSONObj& coorDoc);

    ~ShardingDDLCoordinator();

    /*
     * Check if the given coordinator document has the same options as this.
     *
     * This is used to decide if we can join a previously created coordinator.
     * In the case the given coordinator document has incompatible options with this,
     * this function must throw a ConflictingOprationInProgress exception with an adequate message.
     */
    virtual void checkIfOptionsConflict(const BSONObj& coorDoc) const = 0;


    /*
     * Returns a future that will be completed when the construction of this coordinator instance
     * is completed.
     *
     * In particular the returned future will be ready only after this coordinator succesfully
     * aquires the required locks.
     */
    SharedSemiFuture<void> getConstructionCompletionFuture() {
        return _constructionCompletionPromise.getFuture();
    }

    const NamespaceString& nss() const {
        return _coorMetadata.getId().getNss();
    }

    const ForwardableOperationMetadata& getForwardableOpMetadata() const {
        invariant(_coorMetadata.getForwardableOpMetadata());
        return _coorMetadata.getForwardableOpMetadata().get();
    }

protected:
    ShardingDDLCoordinatorMetadata _coorMetadata;
    std::stack<DistLockManager::ScopedDistLock> _scopedLocks;

private:
    SemiFuture<void> run(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                         const CancelationToken& token) noexcept override final;

    virtual ExecutorFuture<void> _runImpl(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                          const CancelationToken& token) noexcept = 0;

    SharedPromise<void> _constructionCompletionPromise;
};

class ShardingDDLCoordinator_NORESILIENT {
public:
    ShardingDDLCoordinator_NORESILIENT(OperationContext* opCtx, const NamespaceString& nss);
    SemiFuture<void> run(OperationContext* opCtx);

protected:
    NamespaceString _nss;
    ForwardableOperationMetadata _forwardableOpMetadata;

private:
    virtual SemiFuture<void> runImpl(std::shared_ptr<executor::TaskExecutor>) = 0;
};

}  // namespace mongo
