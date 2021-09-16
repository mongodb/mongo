/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/resharding/resharding_oplog_batch_applier.h"

#include <memory>

#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/resharding/resharding_data_copy_util.h"
#include "mongo/db/s/resharding/resharding_future_util.h"
#include "mongo/db/s/resharding/resharding_oplog_application.h"
#include "mongo/db/s/resharding/resharding_oplog_session_application.h"
#include "mongo/logv2/log.h"

namespace mongo {

ReshardingOplogBatchApplier::ReshardingOplogBatchApplier(
    const ReshardingOplogApplicationRules& crudApplication,
    const ReshardingOplogSessionApplication& sessionApplication)
    : _crudApplication(crudApplication), _sessionApplication(sessionApplication) {}

template <bool IsForSessionApplication>
SemiFuture<void> ReshardingOplogBatchApplier::applyBatch(
    OplogBatch batch,
    std::shared_ptr<executor::TaskExecutor> executor,
    CancellationToken cancelToken,
    CancelableOperationContextFactory factory) const {
    struct ChainContext {
        OplogBatch batch;
        size_t nextToApply = 0;
    };

    auto chainCtx = std::make_shared<ChainContext>();
    chainCtx->batch = std::move(batch);

    return resharding::WithAutomaticRetry<unique_function<SemiFuture<void>()>>(
               [this, chainCtx, cancelToken, factory] {
                   // Writing `auto& i = chainCtx->nextToApply` takes care of incrementing
                   // chainCtx->nextToApply on each loop iteration.
                   for (auto& i = chainCtx->nextToApply; i < chainCtx->batch.size(); ++i) {
                       const auto& oplogEntry = *chainCtx->batch[i];
                       auto opCtx = factory.makeOperationContext(&cc());

                       if constexpr (IsForSessionApplication) {
                           auto hitPreparedTxn =
                               _sessionApplication.tryApplyOperation(opCtx.get(), oplogEntry);

                           if (hitPreparedTxn) {
                               return future_util::withCancellation(std::move(*hitPreparedTxn),
                                                                    cancelToken);
                           }
                       } else {
                           // ReshardingOpObserver depends on the collection metadata being known
                           // when processing writes to the temporary resharding collection. We
                           // attach shard version IGNORED to the write operations and retry once
                           // on a StaleConfig exception to allow the collection metadata
                           // information to be recovered.
                           auto& oss = OperationShardingState::get(opCtx.get());
                           oss.initializeClientRoutingVersions(
                               _crudApplication.getOutputNss(),
                               ChunkVersion::IGNORED() /* shardVersion */,
                               boost::none /* dbVersion */);

                           resharding::data_copy::withOneStaleConfigRetry(opCtx.get(), [&] {
                               uassertStatusOK(
                                   _crudApplication.applyOperation(opCtx.get(), oplogEntry));
                           });
                       }
                   }
                   return makeReadyFutureWith([] {}).semi();
               })
        .onTransientError([](const Status& status) {
            LOGV2(5615800,
                  "Transient error while applying oplog entry from donor shard",
                  "error"_attr = redact(status));
        })
        .onUnrecoverableError([](const Status& status) {
            LOGV2_ERROR(
                5615801,
                "Operation-fatal error for resharding while applying oplog entry from donor shard",
                "error"_attr = redact(status));
        })
        .template until<Status>([chainCtx](const Status& status) {
            return status.isOK() && chainCtx->nextToApply >= chainCtx->batch.size();
        })
        .on(std::move(executor), cancelToken)
        // There isn't a guarantee that the reference count to `executor` has been decremented after
        // .on() returns. We schedule a trivial task on the task executor to ensure the callback's
        // destructor has run. Otherwise `executor` could end up outliving the ServiceContext and
        // triggering an invariant due to the task executor's thread having a Client still.
        .onCompletion([](auto x) { return x; })
        .semi();
}

template SemiFuture<void> ReshardingOplogBatchApplier::applyBatch<false>(
    OplogBatch batch,
    std::shared_ptr<executor::TaskExecutor> executor,
    CancellationToken cancelToken,
    CancelableOperationContextFactory factory) const;

template SemiFuture<void> ReshardingOplogBatchApplier::applyBatch<true>(
    OplogBatch batch,
    std::shared_ptr<executor::TaskExecutor> executor,
    CancellationToken cancelToken,
    CancelableOperationContextFactory factory) const;

}  // namespace mongo
