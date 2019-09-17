/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <deque>
#include <vector>

#include "mongo/bson/ordering.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/exchange_spec_gen.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/platform/condition_variable.h"
#include "mongo/platform/mutex.h"

namespace mongo {

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
    Exchange(ExchangeSpec spec, std::unique_ptr<Pipeline, PipelineDeleter> pipeline);

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
    size_t loadNextBatch();

    size_t getTargetConsumer(const Document& input);

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

    // An input to the exchange operator
    std::unique_ptr<Pipeline, PipelineDeleter> _pipeline;

    // Synchronization.
    Mutex _mutex = MONGO_MAKE_LATCH("Exchange::_mutex");
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
};

class DocumentSourceExchange final : public DocumentSource {
public:
    static constexpr StringData kStageName = "$_internalExchange"_sd;

    /**
     * Create an Exchange consumer. 'resourceYielder' is so the exchange may temporarily yield
     * resources (such as the Session) while waiting for other threads to do
     * work. 'resourceYielder' may be nullptr if there are no resources which need to be given up
     * while waiting.
     */
    DocumentSourceExchange(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                           const boost::intrusive_ptr<Exchange> exchange,
                           size_t consumerId,
                           std::unique_ptr<ResourceYielder> yielder);

    StageConstraints constraints(Pipeline::SplitState pipeState) const final {
        return {StreamType::kStreaming,
                PositionRequirement::kNone,
                HostTypeRequirement::kNone,
                DiskUseRequirement::kNoDiskUse,
                FacetRequirement::kAllowed,
                TransactionRequirement::kAllowed,
                LookupRequirement::kNotAllowed};
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final {
        return boost::none;
    }

    const char* getSourceName() const final;

    Value serialize(boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final;

    /**
     * DocumentSourceExchange does not have a direct source (it is reading through the shared
     * Exchange pipeline).
     */
    void setSource(DocumentSource* source) final {
        invariant(!source);
    }

    size_t getConsumers() const {
        return _exchange->getConsumers();
    }

    auto getExchange() const {
        return _exchange;
    }

    void doDispose() final {
        _exchange->dispose(pExpCtx->opCtx, _consumerId);
    }

    auto getConsumerId() const {
        return _consumerId;
    }

private:
    GetNextResult doGetNext() final;

    boost::intrusive_ptr<Exchange> _exchange;

    const size_t _consumerId;

    // While waiting for another thread to make room in its buffer, we may want to yield certain
    // resources (such as the Session). Through this interface we can do that.
    std::unique_ptr<ResourceYielder> _resourceYielder;
};

}  // namespace mongo
