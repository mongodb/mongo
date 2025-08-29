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

#include "mongo/db/exec/agg/exchange_stage.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/pipeline_builder.h"
#include "mongo/db/hasher.h"
#include "mongo/db/pipeline/document_source_exchange.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <iterator>
#include <set>
#include <utility>

#include <boost/cstdint.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

exec::agg::StagePtr documentSourceExchangeToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    auto ds = boost::dynamic_pointer_cast<DocumentSourceExchange>(documentSource);
    tassert(10815901, "expected 'DocumentSourceExchange' type", ds);
    return make_intrusive<exec::agg::ExchangeStage>(
        ds->kStageName, ds->getExpCtx(), ds->_exchange, ds->_consumerId, ds->_resourceYielder);
}
namespace exec::agg {
REGISTER_AGG_STAGE_MAPPING(exchange, DocumentSourceExchange::id, documentSourceExchangeToStageFn)

MONGO_FAIL_POINT_DEFINE(exchangeFailLoadNextBatch);
namespace {

class MutexAndResourceLock {
    OperationContext* _opCtx;
    ResourceYielder* _resourceYielder;
    stdx::unique_lock<stdx::mutex> _lock;

public:
    // Must be constructed with the mutex held. 'yielder' may be null if there are no resources
    // which need to be yielded while waiting.
    MutexAndResourceLock(OperationContext* opCtx,
                         stdx::unique_lock<stdx::mutex> m,
                         ResourceYielder* yielder)
        : _opCtx(opCtx), _resourceYielder(yielder), _lock(std::move(m)) {
        invariant(_lock.owns_lock());
    }

    void lock() {
        // Acquire the operation-wide resources, then the mutex.
        if (_resourceYielder) {
            _resourceYielder->unyield(_opCtx);
        }
        _lock.lock();
    }
    void unlock() {
        _lock.unlock();
        if (_resourceYielder) {
            _resourceYielder->yield(_opCtx);
        }
    }

    /**
     * Releases ownership of the lock to the caller. May only be called when the mutex is held
     * (after a call to unlock(), for example).
     */
    stdx::unique_lock<stdx::mutex> releaseLockOwnership() {
        invariant(_lock.owns_lock());
        return std::move(_lock);
    }
};

}  // namespace

constexpr size_t Exchange::kMaxBufferSize;
constexpr size_t Exchange::kMaxNumberConsumers;

Exchange::Exchange(ExchangeSpec spec, std::unique_ptr<mongo::Pipeline> pipeline)
    : _spec(std::move(spec)),
      _pipeline(std::move(pipeline)),
      _execPipeline{buildPipeline(_pipeline->freeze())},
      _keyPattern(_spec.getKey().getOwned()),
      _ordering(extractOrdering(_keyPattern)),
      _keyPaths(extractKeyPaths(_keyPattern)),
      _boundaries(extractBoundaries(_spec.getBoundaries(), _ordering)),
      _consumerIds(extractConsumerIds(_spec.getConsumerIds(), _spec.getConsumers())),
      _policy(_spec.getPolicy()),
      _orderPreserving(_spec.getOrderPreserving()),
      _maxBufferSize(_spec.getBufferSize()) {
    uassert(50901, "Exchange must have at least one consumer", _spec.getConsumers() > 0);

    uassert(50951,
            str::stream() << "Specified exchange buffer size (" << _maxBufferSize
                          << ") exceeds the maximum allowable amount (" << kMaxBufferSize << ").",
            _maxBufferSize <= kMaxBufferSize);

    for (int idx = 0; idx < _spec.getConsumers(); ++idx) {
        _consumers.emplace_back(std::make_unique<ExchangeBuffer>());
    }

    if (_policy == ExchangePolicyEnum::kKeyRange) {
        uassert(50900,
                "Exchange boundaries do not match number of consumers.",
                _boundaries.size() == _consumerIds.size() + 1);
        uassert(50967,
                str::stream() << "The key pattern " << _keyPattern << " must have at least one key",
                !_keyPaths.empty());
    } else {
        uassert(50899, "Exchange boundaries must not be specified.", _boundaries.empty());
    }

    _execPipeline->detachFromOperationContext();
    _pipeline->detachFromOperationContext();
}

std::vector<std::string> Exchange::extractBoundaries(
    const boost::optional<std::vector<BSONObj>>& obj, Ordering ordering) {
    std::vector<std::string> ret;

    if (!obj) {
        return ret;
    }

    for (auto& b : *obj) {
        key_string::Builder key{key_string::Version::V1, b, ordering};
        ret.emplace_back(key.getView().begin(), key.getView().end());
    }

    uassert(50960, "Exchange range boundaries are not valid", ret.size() > 1);

    for (size_t idx = 1; idx < ret.size(); ++idx) {
        uassert(50893,
                "Exchange range boundaries are not in ascending order.",
                ret[idx - 1] < ret[idx]);
    }

    BSONObjBuilder kbMin;
    BSONObjBuilder kbMax;
    for (int i = 0; i < obj->front().nFields(); ++i) {
        kbMin.appendMinKey("");
        kbMax.appendMaxKey("");
    }

    key_string::Builder minKey{key_string::Version::V1, kbMin.obj(), ordering};
    key_string::Builder maxKey{key_string::Version::V1, kbMax.obj(), ordering};

    uassert(50958,
            "Exchange lower bound must be the minkey.",
            key_string::compare(ret.front(), minKey.getView()) == 0);
    uassert(50959,
            "Exchange upper bound must be the maxkey.",
            key_string::compare(ret.back(), maxKey.getView()) == 0);

    return ret;
}

std::vector<size_t> Exchange::extractConsumerIds(
    const boost::optional<std::vector<std::int32_t>>& consumerIds, size_t nConsumers) {

    uassert(50950,
            str::stream() << "Specified number of exchange consumers (" << nConsumers
                          << ") exceeds the maximum allowable amount (" << kMaxNumberConsumers
                          << ").",
            nConsumers <= kMaxNumberConsumers);

    std::vector<size_t> ret;

    if (!consumerIds) {
        // If the ids are not specified than we generate a simple sequence 0,1,2,3,...
        for (size_t idx = 0; idx < nConsumers; ++idx) {
            ret.push_back(idx);
        }
    } else {
        // Validate that the ids are dense (no hole) and in the range [0,nConsumers)
        std::set<size_t> validation;

        for (auto cid : *consumerIds) {
            validation.insert(cid);
            ret.push_back(cid);
        }

        uassert(50894,
                str::stream() << "Exchange consumers ids are invalid.",
                nConsumers > 0 && validation.size() == nConsumers && *validation.begin() == 0 &&
                    *validation.rbegin() == nConsumers - 1);
    }
    return ret;
}

Ordering Exchange::extractOrdering(const BSONObj& keyPattern) {
    bool hasHashKey = false;
    bool hasOrderKey = false;

    for (const auto& element : keyPattern) {
        if (element.type() == BSONType::string) {
            uassert(50895,
                    str::stream() << "Exchange key description is invalid: " << element,
                    element.valueStringData() == "hashed"_sd);
            hasHashKey = true;
        } else if (element.isNumber()) {
            auto num = element.number();
            if (!(num == 1 || num == -1)) {
                uasserted(50896,
                          str::stream() << "Exchange key description is invalid: " << element);
            }
            hasOrderKey = true;
        } else {
            uasserted(50897, str::stream() << "Exchange key description is invalid: " << element);
        }
    }

    uassert(50898,
            str::stream() << "Exchange hash and order keys cannot be mixed together: "
                          << keyPattern,
            !(hasHashKey && hasOrderKey));

    return hasHashKey ? Ordering::make(BSONObj()) : Ordering::make(keyPattern);
}

std::vector<FieldPath> Exchange::extractKeyPaths(const BSONObj& keyPattern) {
    std::vector<FieldPath> paths;
    for (auto& elem : keyPattern) {
        paths.emplace_back(elem.fieldNameStringData());
    }
    return paths;
}

void Exchange::unblockLoading(size_t consumerId) {
    // See if the loading is blocked on this consumer and if so unblock it.
    if (_loadingThreadId == consumerId) {
        _loadingThreadId = kInvalidThreadId;
        _haveBufferSpace.notify_all();
    }
}
DocumentSource::GetNextResult Exchange::getNext(OperationContext* opCtx,
                                                size_t consumerId,
                                                ResourceYielder* resourceYielder) {
    // Grab a lock.
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    for (;;) {
        // Guard against some of the trickiness we do with moving the lock to/from the
        // MutexAndResourceLock.
        invariant(lk.owns_lock());
        // Execute only in case we have not encountered an error.
        if (!_errorInLoadNextBatch.isOK()) {
            uasserted(ErrorCodes::ExchangePassthrough,
                      "Exchange failed due to an error on different thread.");
        }

        // Check if we have a document.
        if (!_consumers[consumerId]->isEmpty()) {
            auto doc = _consumers[consumerId]->getNext();
            unblockLoading(consumerId);

            return doc;
        }

        // There is not any document so try to load more from the source.
        if (_loadingThreadId == kInvalidThreadId) {
            LOGV2_DEBUG(
                20896, 3, "A consumer {consumerId} begins loading", "consumerId"_attr = consumerId);

            try {
                // This consumer won the race and will fill the buffers.
                _loadingThreadId = consumerId;

                _execPipeline->reattachToOperationContext(opCtx);
                _pipeline->reattachToOperationContext(opCtx);

                // This will return when some exchange buffer is full and we cannot make any forward
                // progress anymore.
                // The return value is an index of a full consumer buffer.
                size_t fullConsumerId = loadNextBatch();

                if (MONGO_unlikely(exchangeFailLoadNextBatch.shouldFail())) {
                    LOGV2(20897, "exchangeFailLoadNextBatch fail point enabled.");
                    uasserted(ErrorCodes::FailPointEnabled,
                              "Asserting on loading the next batch due to failpoint.");
                }

                _execPipeline->detachFromOperationContext();
                _pipeline->detachFromOperationContext();

                // The loading cannot continue until the consumer with the full buffer consumes some
                // documents.
                _loadingThreadId = fullConsumerId;

                // Wake up everybody and try to make some progress.
                _haveBufferSpace.notify_all();
            } catch (const DBException& ex) {
                _errorInLoadNextBatch = ex.toStatus();

                // We have to wake up all other blocked threads so they can detect the error and
                // fail too. They can be woken up only after _errorInLoadNextBatch has been set.
                _haveBufferSpace.notify_all();

                throw;
            }
        } else {
            // Some other consumer is already loading the buffers. There is nothing else we can do
            // but wait.
            MutexAndResourceLock mutexAndResourceLock(opCtx, std::move(lk), resourceYielder);
            _haveBufferSpace.wait(mutexAndResourceLock);
            lk = mutexAndResourceLock.releaseLockOwnership();
        }
    }
}

size_t Exchange::loadNextBatch() {
    auto input = _execPipeline->getNextResult();

    for (; input.isAdvanced(); input = _execPipeline->getNextResult()) {
        // We have a document and we will deliver it to a consumer(s) based on the policy.
        switch (_policy) {
            case ExchangePolicyEnum::kBroadcast: {
                bool full = false;
                // The document is sent to all consumers.
                for (auto& c : _consumers) {
                    // By default the Document is shallow copied. However, the broadcasted document
                    // can be used by multiple threads (consumers) and the Document is not thread
                    // safe. Hence we have to clone the Document.
                    auto copy = DocumentSource::GetNextResult(input.getDocument().clone());
                    full = c->appendDocument(copy, _maxBufferSize);
                }

                if (full)
                    return 0;
            } break;
            case ExchangePolicyEnum::kRoundRobin: {
                size_t target = _roundRobinCounter;
                _roundRobinCounter = (_roundRobinCounter + 1) % _consumers.size();

                if (_consumers[target]->appendDocument(std::move(input), _maxBufferSize))
                    return target;
            } break;
            case ExchangePolicyEnum::kKeyRange: {
                size_t target = getTargetConsumer(input.getDocument());
                bool full = _consumers[target]->appendDocument(std::move(input), _maxBufferSize);
                if (full && _orderPreserving) {
                    // TODO send the high watermark here.
                }
                if (full)
                    return target;
            } break;
            default:
                MONGO_UNREACHABLE;
        }
    }

    invariant(input.isEOF());

    // We have reached the end so send EOS to all consumers.
    for (auto& c : _consumers) {
        [[maybe_unused]] auto full = c->appendDocument(input, _maxBufferSize);
    }

    return kInvalidThreadId;
}

size_t Exchange::getTargetConsumer(const Document& input) {
    // Build the key.
    BSONObjBuilder kb;
    size_t counter = 0;
    for (const auto& elem : _keyPattern) {
        auto value = input.getNestedField(_keyPaths[counter]);

        // By definition we send documents with missing fields to the consumer 0.
        if (value.missing()) {
            return 0;
        }

        if (elem.type() == BSONType::string && elem.str() == "hashed") {
            kb << ""
               << BSONElementHasher::hash64(BSON("" << value).firstElement(),
                                            BSONElementHasher::DEFAULT_HASH_SEED);
        } else {
            kb << "" << value;
        }
        ++counter;
    }

    key_string::Builder key{key_string::Version::V1, kb.obj(), _ordering};
    StringData keyStr{key.getView().data(), key.getView().size()};

    // Binary search for the consumer id.
    auto it = std::upper_bound(_boundaries.begin(), _boundaries.end(), keyStr);
    invariant(it != _boundaries.end());

    size_t distance = std::distance(_boundaries.begin(), it) - 1;
    invariant(distance < _consumerIds.size());

    size_t cid = _consumerIds[distance];
    invariant(cid < _consumers.size());

    return cid;
}

void Exchange::dispose(OperationContext* opCtx, size_t consumerId) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    invariant(_disposeRunDown < getConsumers());

    ++_disposeRunDown;

    // If _errorInLoadNextBatch status is not OK then an exception was thrown. In that case the
    // throwing thread will do the dispose.
    if (!_errorInLoadNextBatch.isOK()) {
        if (_loadingThreadId == consumerId) {
            _execPipeline->reattachToOperationContext(opCtx);
            _execPipeline->dispose();
        }
    } else if (_disposeRunDown == getConsumers()) {
        _execPipeline->reattachToOperationContext(opCtx);
        _execPipeline->dispose();
    }

    _consumers[consumerId]->dispose();
    unblockLoading(consumerId);
}

DocumentSource::GetNextResult Exchange::ExchangeBuffer::getNext() {
    invariant(!_buffer.empty());

    auto result = std::move(_buffer.front());
    _buffer.pop_front();

    if (result.isAdvanced()) {
        _bytesInBuffer -= result.getDocument().getApproximateSize();
    }

    return result;
}

bool Exchange::ExchangeBuffer::appendDocument(DocumentSource::GetNextResult input, size_t limit) {
    // If the buffer is disposed then we simply ignore any appends.
    if (_disposed) {
        return false;
    }

    if (input.isAdvanced()) {
        _bytesInBuffer += input.getDocument().getApproximateSize();
    }
    _buffer.push_back(std::move(input));

    // The buffer is full.
    return _bytesInBuffer >= limit;
}

ExchangeStage::ExchangeStage(StringData stageName,
                             const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                             boost::intrusive_ptr<Exchange> exchange,
                             size_t consumerId,
                             const std::shared_ptr<ResourceYielder>& resourceYielder)
    : Stage(stageName, pExpCtx),
      _exchange(exchange),
      _consumerId(consumerId),
      _resourceYielder(resourceYielder) {}

void ExchangeStage::doDispose() {
    _exchange->dispose(pExpCtx->getOperationContext(), _consumerId);
}

/**
 * ExchangeStage does not have a direct source (it is reading through the shared Exchange
 * pipeline).
 */
void ExchangeStage::setSource(Stage* source) {
    invariant(!source);
}

DocumentSource::GetNextResult ExchangeStage::doGetNext() {
    return _exchange->getNext(pExpCtx->getOperationContext(), _consumerId, _resourceYielder.get());
}

}  // namespace exec::agg
}  // namespace mongo
