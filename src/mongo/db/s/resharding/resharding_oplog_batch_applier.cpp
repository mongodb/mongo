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


#include "mongo/db/s/resharding/resharding_oplog_batch_applier.h"

#include "mongo/base/status.h"
#include "mongo/db/client.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache.h"
#include "mongo/db/local_catalog/shard_role_catalog/operation_sharding_state.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/intent_guard.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/s/resharding/resharding_data_copy_util.h"
#include "mongo/db/s/resharding/resharding_future_util.h"
#include "mongo/db/s/resharding/resharding_oplog_application.h"
#include "mongo/db/s/resharding/resharding_oplog_session_application.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/db/versioning_protocol/shard_version_factory.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/functional.h"
#include "mongo/util/future_util.h"

#include <cstddef>
#include <memory>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResharding


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

                       boost::optional<rss::consensus::WriteIntentGuard> writeGuard;
                       if (gFeatureFlagIntentRegistration.isEnabled()) {
                           writeGuard.emplace(opCtx.get());
                       }

                       if constexpr (IsForSessionApplication) {
                           auto conflictingTxnCompletionFuture =
                               _sessionApplication.tryApplyOperation(opCtx.get(), oplogEntry);

                           if (conflictingTxnCompletionFuture) {
                               return future_util::withCancellation(
                                   std::move(*conflictingTxnCompletionFuture), cancelToken);
                           }
                       } else {
                           if (resharding::isProgressMarkOplogAfterOplogApplicationStarted(
                                   oplogEntry)) {
                               continue;
                           }

                           resharding::data_copy::staleConfigShardLoop(opCtx.get(), [&] {
                               // ReshardingOpObserver depends on the collection metadata being
                               // known when processing writes to the temporary resharding
                               // collection. We attach placement version IGNORED to the write
                               // operations and retry once on a StaleConfig error to allow the
                               // collection metadata information to be recovered.
                               ScopedSetShardRole scopedSetShardRole(
                                   opCtx.get(),
                                   _crudApplication.getOutputNss(),
                                   ShardVersionFactory::make(ChunkVersion::IGNORED()),
                                   boost::none /* databaseVersion */);
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
