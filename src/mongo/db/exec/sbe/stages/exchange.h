/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include <vector>

#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/future.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/future.h"

namespace mongo::sbe {
class ExchangeConsumer;
class ExchangeProducer;

enum class ExchangePolicy { broadcast, roundrobin, partition };

// A unit of exchange between a consumer and a producer
class ExchangeBuffer {
public:
    ~ExchangeBuffer() {
        clear();
    }
    void clear() {
        _eof = false;
        _count = 0;

        for (size_t idx = 0; idx < _typeTags.size(); ++idx) {
            value::releaseValue(_typeTags[idx], _values[idx]);
        }
        _typeTags.clear();
        _values.clear();
    }
    void markEof() {
        _eof = true;
    }
    auto isEof() const {
        return _eof;
    }

    bool appendData(std::vector<value::SlotAccessor*>& data);
    auto count() const {
        return _count;
    }
    // The numbers are arbitrary for now.
    auto isFull() const {
        return _typeTags.size() >= 10240 || _count >= 1024;
    }

    class Accessor : public value::SlotAccessor {
    public:
        void setBuffer(ExchangeBuffer* buffer) {
            _buffer = buffer;
        }
        void setIndex(size_t index) {
            _index = index;
        }

        // Return non-owning view of the value.
        std::pair<value::TypeTags, value::Value> getViewOfValue() const final {
            return {_buffer->_typeTags[_index], _buffer->_values[_index]};
        }
        std::pair<value::TypeTags, value::Value> copyOrMoveValue() final {
            auto tag = _buffer->_typeTags[_index];
            auto val = _buffer->_values[_index];

            _buffer->_typeTags[_index] = value::TypeTags::Nothing;

            return {tag, val};
        }

    private:
        ExchangeBuffer* _buffer{nullptr};
        size_t _index{0};
    };

private:
    std::vector<value::TypeTags> _typeTags;
    std::vector<value::Value> _values;

    // Mark that this is the last buffer.
    bool _eof{false};
    size_t _count{0};

    friend class Accessor;
};

/**
 * A connection that moves data between a consumer and a producer.
 */
class ExchangePipe {
public:
    ExchangePipe(size_t size);

    void close();
    std::unique_ptr<ExchangeBuffer> getEmptyBuffer();
    std::unique_ptr<ExchangeBuffer> getFullBuffer();
    void putEmptyBuffer(std::unique_ptr<ExchangeBuffer>);
    void putFullBuffer(std::unique_ptr<ExchangeBuffer>);

private:
    Mutex _mutex = MONGO_MAKE_LATCH("ExchangePipe::_mutex");
    stdx::condition_variable _cond;

    std::vector<std::unique_ptr<ExchangeBuffer>> _fullBuffers;
    std::vector<std::unique_ptr<ExchangeBuffer>> _emptyBuffers;
    size_t _fullCount{0};
    size_t _fullPosition{0};
    size_t _emptyCount{0};

    // early out - pipe closed
    bool _closed{false};
};

/**
 * Common shared state between all consumers and producers of a single exchange.
 */
class ExchangeState {
public:
    ExchangeState(size_t numOfProducers,
                  value::SlotVector fields,
                  ExchangePolicy policy,
                  std::unique_ptr<EExpression> partition,
                  std::unique_ptr<EExpression> orderLess);

    bool isOrderPreserving() const {
        return !!_orderLess;
    }
    auto policy() const {
        return _policy;
    }

    size_t addConsumer(ExchangeConsumer* c) {
        _consumers.push_back(c);
        return _consumers.size() - 1;
    }

    size_t addProducer(ExchangeProducer* p) {
        _producers.push_back(p);
        return _producers.size() - 1;
    }

    void addProducerFuture(Future<void> f) {
        _producerResults.emplace_back(std::move(f));
    }

    auto& consumerOpenMutex() {
        return _consumerOpenMutex;
    }
    auto& consumerOpenCond() {
        return _consumerOpenCond;
    }
    auto& consumerOpen() {
        return _consumerOpen;
    }

    auto& consumerCloseMutex() {
        return _consumerCloseMutex;
    }
    auto& consumerCloseCond() {
        return _consumerCloseCond;
    }
    auto& consumerClose() {
        return _consumerClose;
    }

    auto& producerPlans() {
        return _producerPlans;
    }

    auto& producerCompileCtxs() {
        return _producerCompileCtxs;
    }

    auto& producerResults() {
        return _producerResults;
    }

    auto numOfConsumers() const {
        return _consumers.size();
    }
    auto numOfProducers() const {
        return _numOfProducers;
    }

    auto& fields() const {
        return _fields;
    }
    ExchangePipe* pipe(size_t consumerTid, size_t producerTid);

private:
    const ExchangePolicy _policy;
    const size_t _numOfProducers;
    std::vector<ExchangeConsumer*> _consumers;
    std::vector<ExchangeProducer*> _producers;
    std::vector<std::unique_ptr<PlanStage>> _producerPlans;
    std::vector<CompileCtx> _producerCompileCtxs;
    std::vector<Future<void>> _producerResults;

    // Variables (fields) that pass through the exchange.
    const value::SlotVector _fields;

    // Partitioning function.
    const std::unique_ptr<EExpression> _partition;

    // The '<' function for order preserving exchange.
    const std::unique_ptr<EExpression> _orderLess;

    // This is verbose and heavyweight. Recondsider something lighter
    // at minimum try to share a single mutex (i.e. _stateMutex) if safe
    mongo::Mutex _consumerOpenMutex;
    stdx::condition_variable _consumerOpenCond;
    size_t _consumerOpen{0};

    mongo::Mutex _consumerCloseMutex;
    stdx::condition_variable _consumerCloseCond;
    size_t _consumerClose{0};
};

class ExchangeConsumer final : public PlanStage {
public:
    ExchangeConsumer(std::unique_ptr<PlanStage> input,
                     size_t numOfProducers,
                     value::SlotVector fields,
                     ExchangePolicy policy,
                     std::unique_ptr<EExpression> partition,
                     std::unique_ptr<EExpression> orderLess);

    ExchangeConsumer(std::shared_ptr<ExchangeState> state);

    std::unique_ptr<PlanStage> clone() const final;

    void prepare(CompileCtx& ctx) final;
    value::SlotAccessor* getAccessor(CompileCtx& ctx, value::SlotId slot) final;
    void open(bool reOpen) final;
    PlanState getNext() final;
    void close() final;

    std::unique_ptr<PlanStageStats> getStats() const final;
    const SpecificStats* getSpecificStats() const final;
    std::vector<DebugPrinter::Block> debugPrint() const final;

    ExchangePipe* pipe(size_t producerTid);

private:
    ExchangeBuffer* getBuffer(size_t producerId);
    void putBuffer(size_t producerId);

    std::shared_ptr<ExchangeState> _state;
    size_t _tid{0};

    // Accessors for the outgoing values (from the exchange buffers).
    std::vector<ExchangeBuffer::Accessor> _outgoing;

    // Pipes to producers (if order preserving) or a single pipe shared by all producers.
    std::vector<std::unique_ptr<ExchangePipe>> _pipes;

    // Current full buffers that this consumer is processing.
    std::vector<std::unique_ptr<ExchangeBuffer>> _fullBuffers;

    // Current position in buffers that this consumer is processing.
    std::vector<size_t> _bufferPos;

    // Count how may EOFs we have seen so far.
    size_t _eofs{0};

    bool _orderPreserving{false};

    size_t _rowProcessed{0};
};

class ExchangeProducer final : public PlanStage {
public:
    ExchangeProducer(std::unique_ptr<PlanStage> input, std::shared_ptr<ExchangeState> state);

    static void start(OperationContext* opCtx,
                      CompileCtx& ctx,
                      std::unique_ptr<PlanStage> producer);

    std::unique_ptr<PlanStage> clone() const final;

    void prepare(CompileCtx& ctx) final;
    value::SlotAccessor* getAccessor(CompileCtx& ctx, value::SlotId slot) final;
    void open(bool reOpen) final;
    PlanState getNext() final;
    void close() final;

    std::unique_ptr<PlanStageStats> getStats() const final;
    const SpecificStats* getSpecificStats() const final;
    std::vector<DebugPrinter::Block> debugPrint() const final;

private:
    ExchangeBuffer* getBuffer(size_t consumerId);
    void putBuffer(size_t consumerId);

    void closePipes();
    bool appendData(size_t consumerId);

    std::shared_ptr<ExchangeState> _state;
    size_t _tid{0};
    size_t _roundRobinCounter{0};

    std::vector<value::SlotAccessor*> _incoming;

    std::vector<ExchangePipe*> _pipes;

    // Current empty buffers that this producer is processing.
    std::vector<std::unique_ptr<ExchangeBuffer>> _emptyBuffers;
};
}  // namespace mongo::sbe
