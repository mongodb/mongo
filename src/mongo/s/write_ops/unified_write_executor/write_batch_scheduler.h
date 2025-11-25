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

#pragma once

#include "mongo/s/write_ops/unified_write_executor/write_batch_executor.h"
#include "mongo/s/write_ops/unified_write_executor/write_batch_response_processor.h"
#include "mongo/s/write_ops/unified_write_executor/write_op_batcher.h"
#include "mongo/util/modules.h"

namespace mongo {
namespace unified_write_executor {

class WriteBatchScheduler {
public:
    using CollectionsToCreate = ProcessorResult::CollectionsToCreate;

    static constexpr size_t kMaxRoundsWithoutProgress = 10;

    WriteBatchScheduler(WriteCommandRef cmdRef,
                        WriteOpBatcher& batcher,
                        WriteBatchExecutor& executor,
                        WriteBatchResponseProcessor& processor,
                        boost::optional<OID> targetEpoch)
        : _cmdRef(std::move(cmdRef)),
          _nssSet(_cmdRef.getNssSet()),
          _batcher(batcher),
          _executor(executor),
          _processor(processor),
          _targetEpoch(targetEpoch) {}

    void run(OperationContext* opCtx);

protected:
    /**
     * Helper method for executing a batch and processing the responses. Returns a boolean that
     * indicates if any progress was made.
     *
     * This method takes care of marking ops for re-processing as needed, it will create collections
     * as needed if there were any CannotImplicitlyCreateCollection errors, and it will also inform
     * the batcher to stop making batches if an unrecoverable error has occurred.
     */
    bool executeRound(OperationContext* opCtx);

    /**
     * Helper method for creating a RoutingContext.
     */
    StatusWith<std::unique_ptr<RoutingContext>> initRoutingContext(
        OperationContext* opCtx, const std::vector<NamespaceString>& nssList);

    /**
     * Helper method for handling the case where RoutingContext creation failed.
     */
    void handleInitRoutingContextError(OperationContext* opCtx, const Status& status);

    /**
     * This method is records errors for the remaining ops. If the write command is ordered or
     * running in a transaction, this method will only record one error (for the remaining op with
     * the lowest ID). Otherwise, this method will record errors for all remaining ops.
     */
    void recordErrorForRemainingOps(OperationContext* opCtx, const Status& status);

    /**
     * Helper method that calls getNextBatch(), handles target errors that occurred during batch
     * creation (if any), and then returns the next batch.
     */
    WriteBatch getNextBatchAndHandleTargetErrors(OperationContext* opCtx,
                                                 RoutingContext& routingCtx);

    /**
     * This method calls release() on 'routingCtx' for the appropriate namespaces in preparation
     * for executing the specified 'batch'.
     */
    void prepareRoutingContext(RoutingContext& routingCtx,
                               const std::vector<NamespaceString>& nssList,
                               const WriteBatch& batch);

    /**
     * Helper method for creating a collection.
     */
    bool createCollection(OperationContext* opCtx, const NamespaceString& nss);

    /**
     * Helper method for creating multiple collections.
     */
    bool createCollections(OperationContext* opCtx, const CollectionsToCreate& collsToCreate);

    const WriteCommandRef _cmdRef;
    const std::set<NamespaceString> _nssSet;
    WriteOpBatcher& _batcher;
    WriteBatchExecutor& _executor;
    WriteBatchResponseProcessor& _processor;
    boost::optional<OID> _targetEpoch;
};

}  // namespace unified_write_executor
}  // namespace mongo
