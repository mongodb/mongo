/**
 *    Copyright (C) 2018 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include <algorithm>
#include <iterator>

#include "mongo/db/pipeline/document_source_exchange.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/util/log.h"

namespace mongo {

REGISTER_DOCUMENT_SOURCE(exchange,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceExchange::createFromBson);

const char* DocumentSourceExchange::getSourceName() const {
    return "$exchange";
}

boost::intrusive_ptr<DocumentSource> DocumentSourceExchange::createFromBson(
    BSONElement spec, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "$exchange options must be specified in an object, but found: "
                          << typeName(spec.type()),
            spec.type() == BSONType::Object);

    IDLParserErrorContext ctx("$exchange");
    auto parsed = ExchangeSpec::parse(ctx, spec.embeddedObject());

    boost::intrusive_ptr<Exchange> exchange = new Exchange(parsed);

    return new DocumentSourceExchange(pExpCtx, exchange, 0);
}

Value DocumentSourceExchange::serialize(boost::optional<ExplainOptions::Verbosity> explain) const {
    return Value(DOC(getSourceName() << _exchange->getSpec().toBSON()));
}

DocumentSourceExchange::DocumentSourceExchange(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const boost::intrusive_ptr<Exchange> exchange,
    size_t consumerId)
    : DocumentSource(expCtx), _exchange(exchange), _consumerId(consumerId) {}

DocumentSource::GetNextResult DocumentSourceExchange::getNext() {
    return _exchange->getNext(_consumerId);
}

Exchange::Exchange(const ExchangeSpec& spec)
    : _spec(spec),
      _keyPattern(spec.getKey().getOwned()),
      _boundaries(extractBoundaries(spec.getBoundaries())),
      _policy(spec.getPolicy()),
      _orderPreserving(spec.getOrderPreserving()),
      _maxBufferSize(spec.getBufferSize()) {
    for (int idx = 0; idx < spec.getConsumers(); ++idx) {
        _consumers.emplace_back(std::make_unique<ExchangeBuffer>());
    }

    if (_policy == ExchangePolicyEnum::kRange) {
        uassert(ErrorCodes::BadValue,
                "$exchange boundaries do not much number of consumers.",
                getConsumers() + 1 == _boundaries.size());
    } else if (_policy == ExchangePolicyEnum::kHash) {
        uasserted(ErrorCodes::BadValue, "$exchange hash is not yet implemented.");
    }
}

std::vector<std::string> Exchange::extractBoundaries(
    const boost::optional<std::vector<BSONObj>>& obj) {
    std::vector<std::string> ret;

    if (!obj) {
        return ret;
    }

    for (auto& b : *obj) {
        // Build the key.
        BSONObjBuilder kb;
        for (auto elem : b) {
            kb << "" << elem;
        }

        KeyString key{KeyString::Version::V1, kb.obj(), Ordering::make(BSONObj())};
        std::string keyStr{key.getBuffer(), key.getSize()};

        ret.emplace_back(std::move(keyStr));
    }

    for (size_t idx = 1; idx < ret.size(); ++idx) {
        uassert(ErrorCodes::BadValue,
                str::stream() << "$exchange range boundaries are not in ascending order.",
                ret[idx - 1] < ret[idx]);
    }
    return ret;
}

DocumentSource::GetNextResult Exchange::getNext(size_t consumerId) {
    // Grab a lock.
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    for (;;) {
        // Check if we have a document.
        if (!_consumers[consumerId]->isEmpty()) {
            auto doc = _consumers[consumerId]->getNext();

            // See if the loading is blocked on this consumer and if so unblock it.
            if (_loadingThreadId == consumerId) {
                _loadingThreadId = kInvalidThreadId;
                _haveBufferSpace.notify_all();
            }

            return doc;
        }

        // There is not any document so try to load more from the source.
        if (_loadingThreadId == kInvalidThreadId) {
            LOG(3) << "A consumer " << consumerId << " begins loading";

            // This consumer won the race and will fill the buffers.
            _loadingThreadId = consumerId;

            // This will return when some exchange buffer is full and we cannot make any forward
            // progress anymore.
            // The return value is an index of a full consumer buffer.
            size_t fullConsumerId = loadNextBatch();

            // The loading cannot continue until the consumer with the full buffer consumes some
            // documents.
            _loadingThreadId = fullConsumerId;

            // Wake up everybody and try to make some progress.
            _haveBufferSpace.notify_all();
        } else {
            // Some other consumer is already loading the buffers. There is nothing else we can do
            // but wait.
            _haveBufferSpace.wait(lk);
        }
    }
}

size_t Exchange::loadNextBatch() {
    auto input = pSource->getNext();

    for (; input.isAdvanced(); input = pSource->getNext()) {
        // We have a document and we will deliver it to a consumer(s) based on the policy.
        switch (_policy) {
            case ExchangePolicyEnum::kBroadcast: {
                bool full = false;
                // The document is sent to all consumers.
                for (auto& c : _consumers) {
                    full = c->appendDocument(input, _maxBufferSize);
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
            case ExchangePolicyEnum::kRange: {
                size_t target = getTargetConsumer(input.getDocument());
                bool full = _consumers[target]->appendDocument(std::move(input), _maxBufferSize);
                if (full && _orderPreserving) {
                    // TODO send the high watermark here.
                }
                if (full)
                    return target;
            } break;
            case ExchangePolicyEnum::kHash: {
                // TODO implement the hash policy. Note that returning 0 is technically correct.
                size_t target = 0;
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
        c->appendDocument(input, _maxBufferSize);
    }

    return kInvalidThreadId;
}

size_t Exchange::getTargetConsumer(const Document& input) {
    // Build the key.
    BSONObjBuilder kb;
    for (auto elem : _keyPattern) {
        auto value = input[elem.fieldName()];
        kb << "" << value;
    }

    // TODO implement hash keys for the hash policy.
    KeyString key{KeyString::Version::V1, kb.obj(), Ordering::make(BSONObj())};
    std::string keyStr{key.getBuffer(), key.getSize()};

    // Binary search for the consumer id.
    auto it = std::upper_bound(_boundaries.begin(), _boundaries.end(), keyStr);
    invariant(it != _boundaries.end());

    size_t distance = std::distance(_boundaries.begin(), it) - 1;
    invariant(distance < _consumers.size());

    return distance;
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
    if (input.isAdvanced()) {
        _bytesInBuffer += input.getDocument().getApproximateSize();
    }
    _buffer.push_back(std::move(input));

    // The buffer is full.
    return _bytesInBuffer >= limit;
}

}  // namespace mongo
