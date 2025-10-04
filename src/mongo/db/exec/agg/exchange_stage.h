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

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/ordering.h"
#include "mongo/db/exec/agg/exec_pipeline.h"
#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/local_catalog/shard_role_api/resource_yielder.h"
#include "mongo/db/memory_tracking/operation_memory_usage_tracker.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/exchange_spec_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>


namespace mongo {
namespace exec {
namespace agg {

/**
 * Exchange operator implementation. The state in this class is shared between the consumer threads.
 * TODO: SERVER-108866 don't expose this operator beyond ExchangeStage code.
 **/
class Exchange : public RefCountable {
    static constexpr size_t kInvalidThreadId{std::numeric_limits<size_t>::max()};
    static constexpr size_t kMaxBufferSize = 100 * 1024 * 1024;  // 100 MB
    static constexpr size_t kMaxNumberConsumers = 100;

    /**
     * Convert the BSON representation of boundaries (as deserialized off the wire) to the internal
     * format (KeyString).
     */
    static std::vector<std::string> extractBoundaries(
        const boost::optional<std::vector<BSONObj>>& obj, Ordering ordering);

    /**
     * Validate consumer ids coming off the wire. If the ids pass the validation then return them.
     * If the ids are not provided (boost::none) then generate a sequence [0..nConsumer-1].
     */
    static std::vector<size_t> extractConsumerIds(
        const boost::optional<std::vector<std::int32_t>>& consumerIds, size_t nConsumers);

    /**
     * Extract the order description from the key.
     */
    static Ordering extractOrdering(const BSONObj& keyPattern);

    /**
     * Extract dotted paths from the key.
     */
    static std::vector<FieldPath> extractKeyPaths(const BSONObj& keyPattern);

public:
    /**
     * Create an exchange. 'pipeline' represents the input to the exchange operator and must not be
     * nullptr.
     **/
    Exchange(OperationContext* opCtx, ExchangeSpec spec, std::unique_ptr<mongo::Pipeline> pipeline);

    /**
     * Interface for retrieving the next document. 'resourceYielder' is optional, and if provided,
     * will be used to give up resources while waiting for other threads to empty their buffers.
     */
    DocumentSource::GetNextResult getNext(OperationContext* opCtx,
                                          size_t consumerId,
                                          ResourceYielder* resourceYielder);

    size_t getConsumers() const {
        return _consumers.size();
    }

    auto& getSpec() const {
        return _spec;
    }

    void dispose(OperationContext* opCtx, size_t consumerId);

    /**
     * Unblocks the loading thread (a producer) if the loading is blocked by a consumer identified
     * by consumerId. Note that there is no such thing as being blocked by multiple consumers. It is
     * always one consumer that blocks the loading (i.e. the consumer's buffer is full and we can
     * not append new documents). The unblocking happens when a consumer consumes some documents
     * from its buffer (i.e. making room for appends) or when a consumer is disposed.
     */
    void unblockLoading(size_t consumerId);

private:
    /**
     * Attaches the subpipeline to the given opCtx. If consumerId is zero, also attaches the
     * OperationMemoryUsageTracker to the current opCtx, so memory metrics are reported to CurOp.
     */
    void attachContext(OperationContext* opCtx, size_t consumerId);

    /**
     * Execute the subpipeline and fill the exchange buffers until one of the buffers is full.
     * Returns the consumerId of the full buffer.
     */
    size_t loadNextBatch();

    /**
     * Detaches the subpipeline from its opCtx. If consumerId is zero, moves the
     * OperationMemoryUsageTracker from the opCtx back to the Exchange object.
     */
    void detachContext(OperationContext* opCtx, size_t consumerId);

    size_t getTargetConsumer(const Document& input);

    /**
     * For consumerId zero, updates CurOp with the current memory metrics in
     * OperationMemoryUsageTracker.
     */
    void updateMemoryTrackingForDispose(OperationContext* opCtx);

    class ExchangeBuffer {
    public:
        bool appendDocument(DocumentSource::GetNextResult input, size_t limit);
        DocumentSource::GetNextResult getNext();
        bool isEmpty() const {
            return _buffer.empty();
        }
        /**
         * Mark the buffer associated with a consumer as disposed. After calling this method,
         * subsequent results that are appended to this buffer are instead discarded to prevent this
         * unused buffer from filling up and blocking progress on other threads.
         */
        void dispose() {
            invariant(!_disposed);
            _disposed = true;
            _buffer.clear();
            _bytesInBuffer = 0;
        }

    private:
        size_t _bytesInBuffer{0};
        std::deque<DocumentSource::GetNextResult> _buffer;
        bool _disposed{false};
    };

    // Keep a copy of the spec for serialization purposes.
    const ExchangeSpec _spec;

    // An input to the exchange operator
    std::unique_ptr<mongo::Pipeline> _pipeline;
    std::unique_ptr<exec::agg::Pipeline> _execPipeline;

    // A pattern for extracting a key from a document used by range and hash policies.
    const BSONObj _keyPattern;

    const Ordering _ordering;

    const std::vector<FieldPath> _keyPaths;

    // Range boundaries. The boundaries are ordered and must cover the whole domain, e.g.
    // [Min, -200, 0, 200, Max] partitions the domain into 4 ranges (i.e. 1 less than number of
    // boundaries). Every range has an assigned consumer that will process documents in that range.
    const std::vector<std::string> _boundaries;

    // A mapping from the range to consumer id. For the ranges from the example above the array must
    // have 4 elements (1 for every range):
    // [0, 1, 0, 1]
    // consumer 0 processes ranges 1 and 3 (i.e. [Min,-200] and [0,200])
    // consumer 1 processes ranges 2 and 4 (i.e. [-200,0] and [200,Max])
    const std::vector<size_t> _consumerIds;

    // A policy that tells how to distribute input documents to consumers.
    const ExchangePolicyEnum _policy;

    // If set to true then a producer sends special 'high watermark' documents to consumers in order
    // to prevent deadlocks.
    const bool _orderPreserving;

    // A maximum size of buffer per consumer.
    const size_t _maxBufferSize;

    // Synchronization.
    stdx::mutex _mutex;
    stdx::condition_variable _haveBufferSpace;

    // A thread that is currently loading the exchange buffers.
    size_t _loadingThreadId{kInvalidThreadId};

    // A status indicating that the exception was thrown during loadNextBatch(). Once in the failed
    // state all other producing threads will fail too.
    Status _errorInLoadNextBatch{Status::OK()};

    size_t _roundRobinCounter{0};

    // A rundown counter of consumers disposing of the pipelines. Only the last consumer will
    // dispose of the 'inner' exchange pipeline.
    size_t _disposeRunDown{0};

    std::vector<std::unique_ptr<ExchangeBuffer>> _consumers;

    // The OperationMemoryTracker for the exchange pipeline. Stages in the subpipeline that track
    // memory will report to this memory tracker. Except when consumer 0 is executing the
    // subpipeline, the operation memory tracker is stored here and not attached to any particular
    // OperationContext. We do this to avoid data races that would occur if we were to move the
    // memory tracker between the OperationContext and ClientCursor while a pipeline is executing.
    std::unique_ptr<OperationMemoryUsageTracker> _memoryTracker;
};

/**
 * This class handles the execution part of internal exchange stage and
 * is part of the execution pipeline. Its construction is based on
 * DocumentSourceSingleDocumentTransformation, which handles the optimization part.
 *
 * This stage is very unusual in that it is executed by multiple threads in
 * parallel. The execution flow goes like this:
 * - An aggregate request with an exchange spec arrives at a mongod. Example:
 *   {
 *     aggregate: collName,
 *     pipeline: [{ $group: ... }],
 *     cursor: {batchSize: 0},
 *     exchange: {
 *       "policy": "roundrobin",
 *       "consumers": 5,
 *       "orderPreserving": false,
 *       "bufferSize": NumberInt(128),
 *       "key": {}
 *     }
 *   }
 * - The response generated by mongod will have 5 cursors one for each consumer.
 * - Each cursor will have an executor whose pipeline is an ExchangeStage.
 *   All 5 ExchangeStages will point at the same Exchange object (member _exchange).
 * - The Exchange object has synchronization members as well as an attached subpipeline. All 5
 *   consumers cooperatively execute the pipeline to buffer documents to be consumed according the
 *   exchange spec's policy.
 *
 * ExchangeStage presents some challenges for tracking memory:
 * - TODO SERVER-110500: Idle cursors for exchange pipeline will not show memory metrics
 * - TODO SERVER-109511: Implement memory tracking for exchange stage buffered documents
 * - TODO SERVER-110499: Allow tracking memory for stages after Exchange
 *
 * The current approach avoids data races between the first consumer (consumerId 0) and the other
 * threads by attaching the OperationMemoryUsageTracker to the shared Exchange object, rather than a
 * consumer's cursor and opCtx. However, this means that we don't report memory metrics for
 * consumer 0 when its cursor is idle. This is SERVER-110500.
 */
class ExchangeStage final : public Stage {
public:
    ExchangeStage(StringData stageName,
                  const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                  boost::intrusive_ptr<Exchange> exchange,
                  size_t consumerId,
                  const std::shared_ptr<ResourceYielder>& resourceYielder);

    void setSource(Stage* source) final;

private:
    void doDispose() final;
    GetNextResult doGetNext() final;

    boost::intrusive_ptr<Exchange> _exchange;
    const size_t _consumerId;

    // While waiting for another thread to make room in its buffer, we may want to yield certain
    // resources (such as the Session). Through this interface we can do that.
    std::shared_ptr<ResourceYielder> _resourceYielder;
};
}  // namespace agg
}  // namespace exec
}  // namespace mongo
