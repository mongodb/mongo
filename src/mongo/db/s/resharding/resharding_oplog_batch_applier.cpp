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

#include "mongo/platform/basic.h"

#include "mongo/db/s/resharding/resharding_oplog_batch_applier.h"

#include <memory>

#include "mongo/db/s/resharding/resharding_oplog_application.h"
#include "mongo/db/s/resharding/resharding_oplog_session_application.h"
#include "mongo/util/future_util.h"

namespace mongo {

ReshardingOplogBatchApplier::ReshardingOplogBatchApplier(
    const ReshardingOplogApplicationRules& crudApplication,
    const ReshardingOplogSessionApplication& sessionApplication)
    : _crudApplication(crudApplication), _sessionApplication(sessionApplication) {}

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

    return AsyncTry([this, factory, chainCtx] {
               // Writing `auto& i = chainCtx->nextToApply` takes care of incrementing
               // chainCtx->nextToApply on each loop iteration.
               for (auto& i = chainCtx->nextToApply; i < chainCtx->batch.size(); ++i) {
                   const auto& oplogEntry = *chainCtx->batch[i];
                   auto opCtx = factory.makeOperationContext(&cc());

                   if (oplogEntry.isForReshardingSessionApplication()) {
                       auto hitPreparedTxn =
                           _sessionApplication.tryApplyOperation(opCtx.get(), oplogEntry);

                       if (hitPreparedTxn) {
                           return *hitPreparedTxn;
                       }
                   } else {
                       uassertStatusOK(_crudApplication.applyOperation(opCtx.get(), oplogEntry));
                   }
               }
               return makeReadyFutureWith([] {}).share();
           })
        .until([chainCtx](Status status) {
            return !status.isOK() || chainCtx->nextToApply >= chainCtx->batch.size();
        })
        .on(std::move(executor), std::move(cancelToken))
        .semi();
}

}  // namespace mongo
