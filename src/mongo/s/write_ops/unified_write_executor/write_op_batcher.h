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

#include "mongo/s/write_ops/unified_write_executor/write_op_analyzer.h"
#include "mongo/s/write_ops/unified_write_executor/write_op_producer.h"

#include <boost/optional.hpp>

namespace mongo {
namespace unified_write_executor {

struct SimpleWriteBatch {
    // Given that a write command can target multiple collections,
    // we store one shard version per namespace to support batching ops which target the same shard,
    // but target different namespaces.
    struct ShardRequest {
        std::map<NamespaceString, ShardEndpoint> versionByNss;
        std::vector<WriteOp> ops;
    };
    std::map<ShardId, ShardRequest> requestByShardId;
};

using WriteBatch = std::variant<SimpleWriteBatch>;

/**
 * Based on the analysis of the write ops, this class bundles multiple write ops into batches to be
 * sent to shards.
 */
class WriteOpBatcher {
public:
    WriteOpBatcher(WriteOpProducer& producer, WriteOpAnalyzer& analyzer)
        : _producer(producer), _analyzer(analyzer) {}

    virtual ~WriteOpBatcher() = default;

    /**
     * Get the next batch from the list of write ops returned by the producer. Based on the analysis
     * results, the batches may be of different write types. If there are no more write ops to be
     * batched up, this function returns none.
     */
    virtual boost::optional<WriteBatch> getNextBatch(OperationContext* opCtx,
                                                     const RoutingContext& routingCtx) = 0;

    /**
     * Mark a list of write ops to be reprocessed, which will in turn be reanalyzed and rebatched.
     */
    void markOpReprocess(const std::vector<WriteOp> ops) {
        for (const auto& op : ops) {
            _producer.markOpReprocess(op);
        }
    }

    /**
     * Mark an unrecoverable error has occurred, for ordered batcher this means no further batches
     * should be produced.
     */
    virtual void markUnrecoverableError() = 0;

protected:
    WriteOpProducer& _producer;
    WriteOpAnalyzer& _analyzer;
};

class OrderedWriteOpBatcher : public WriteOpBatcher {
public:
    OrderedWriteOpBatcher(WriteOpProducer& producer, WriteOpAnalyzer& analyzer)
        : WriteOpBatcher(producer, analyzer) {}

    boost::optional<WriteBatch> getNextBatch(OperationContext* opCtx,
                                             const RoutingContext& routingCtx) override;

    void markUnrecoverableError() override;

private:
    bool unrecoverableError{false};
};

class UnorderedWriteOpBatcher : public WriteOpBatcher {
public:
    UnorderedWriteOpBatcher(WriteOpProducer& producer, WriteOpAnalyzer& analyzer)
        : WriteOpBatcher(producer, analyzer) {}

    boost::optional<WriteBatch> getNextBatch(OperationContext* opCtx,
                                             const RoutingContext& routingCtx) override;

    void markUnrecoverableError() override {};
};

}  // namespace unified_write_executor
}  // namespace mongo
