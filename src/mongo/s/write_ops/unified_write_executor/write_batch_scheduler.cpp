/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/s/write_ops/unified_write_executor/write_batch_scheduler.h"

#include "mongo/s/cluster_ddl.h"

namespace mongo {
namespace unified_write_executor {

void WriteBatchScheduler::run(OperationContext* opCtx, const std::set<NamespaceString>& nssSet) {
    while (!_batcher.isDone()) {

        // Destroy the routing context after retrieving the next batch, as later processing
        // may generate separate routing operations (e.g. implicitly creating collections)
        // but at most one routing context should exist in one thread at any given time.
        auto collsToCreate = routing_context_utils::withValidatedRoutingContextForTxnCmd(
            opCtx,
            std::vector<NamespaceString>{nssSet.begin(), nssSet.end()},
            [&,
             this](RoutingContext& routingCtx) -> WriteBatchResponseProcessor::CollectionsToCreate {
                auto batchOfRequests = _batcher.getNextBatch(opCtx, routingCtx);
                tassert(10411402,
                        "batcher has no batches left but 'isDone()' returned false",
                        batchOfRequests.has_value());


                // Dismiss validation for any namespaces that don't have write ops in this
                // round.
                auto involvedNamespaces = batchOfRequests->getInvolvedNamespaces();
                for (const auto& nss : nssSet) {
                    if (!involvedNamespaces.contains(nss)) {
                        routingCtx.release(nss);
                    }
                }


                auto batchOfResponses = _executor.execute(opCtx, routingCtx, *batchOfRequests);
                auto result = _processor.onWriteBatchResponse(routingCtx, batchOfResponses);
                if (result.unrecoverableError) {
                    _batcher.markUnrecoverableError();
                }
                if (!result.opsToRetry.empty()) {
                    _batcher.markOpReprocess(result.opsToRetry);
                }
                return result.collsToCreate;
            });
        if (!collsToCreate.empty()) {
            for (auto& [nss, _] : collsToCreate) {
                cluster::createCollectionWithRouterLoop(opCtx, nss);
            }
        }
    }
}

}  // namespace unified_write_executor
}  // namespace mongo
