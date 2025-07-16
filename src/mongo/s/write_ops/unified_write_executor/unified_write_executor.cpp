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

#include "mongo/s/write_ops/unified_write_executor/unified_write_executor.h"

#include "mongo/s/write_ops/unified_write_executor/write_batch_executor.h"
#include "mongo/s/write_ops/unified_write_executor/write_batch_response_processor.h"
#include "mongo/s/write_ops/unified_write_executor/write_batch_scheduler.h"
#include "mongo/s/write_ops/unified_write_executor/write_op_batcher.h"
#include "mongo/s/write_ops/unified_write_executor/write_op_producer.h"

namespace mongo {
namespace unified_write_executor {

template <typename ResponseType, typename RequestType>
ResponseType execWriteRequest(OperationContext* opCtx, const RequestType& request) {
    WriteOpContext context(request);
    MultiWriteOpProducer<RequestType> producer(request);
    WriteOpAnalyzer analyzer;

    std::set<NamespaceString> nssSet;
    bool ordered = false;
    if constexpr (std::is_same_v<RequestType, BatchedCommandRequest>) {
        nssSet.insert(request.getNS());
        ordered = request.getWriteCommandRequestBase().getOrdered();
    } else {
        static_assert(std::is_same_v<RequestType, BulkWriteCommandRequest>);
        for (const auto& nsInfo : request.getNsInfo()) {
            nssSet.insert(nsInfo.getNs());
        }
        ordered = request.getOrdered();
    }

    std::unique_ptr<WriteOpBatcher> batcher{nullptr};
    if (ordered) {
        batcher = std::make_unique<OrderedWriteOpBatcher>(producer, analyzer);
    } else {
        batcher = std::make_unique<UnorderedWriteOpBatcher>(producer, analyzer);
    }
    WriteBatchExecutor executor(context);
    WriteBatchResponseProcessor processor(context);
    WriteBatchScheduler scheduler(*batcher, executor, processor);


    scheduler.run(opCtx, nssSet);
    return processor.generateClientResponse<ResponseType>();
}

template BatchedCommandResponse execWriteRequest<BatchedCommandResponse, BatchedCommandRequest>(
    OperationContext*, const BatchedCommandRequest&);
template BulkWriteCommandReply execWriteRequest<BulkWriteCommandReply, BulkWriteCommandRequest>(
    OperationContext*, const BulkWriteCommandRequest&);
}  // namespace unified_write_executor
}  // namespace mongo
