// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/tee_buffer.h"

#include "mongo/db/exec/document_value/document.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <utility>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

TeeBuffer::TeeBuffer(size_t nConsumers, size_t bufferSizeBytes)
    : _bufferSizeBytes(bufferSizeBytes), _consumers(nConsumers) {}

boost::intrusive_ptr<TeeBuffer> TeeBuffer::create(size_t nConsumers, int bufferSizeBytes) {
    uassert(40309, "need at least one consumer for a TeeBuffer", nConsumers > 0);
    uassert(40310,
            str::stream() << "TeeBuffer requires a positive buffer size, was given "
                          << bufferSizeBytes,
            bufferSizeBytes > 0);
    return new TeeBuffer(nConsumers, bufferSizeBytes);
}

DocumentSource::GetNextResult TeeBuffer::getNext(size_t consumerId) {
    size_t nConsumersStillProcessingThisBatch =
        std::count_if(_consumers.begin(), _consumers.end(), [](const ConsumerInfo& info) {
            return info.nLeftToReturn > 0;
        });

    if (_buffer.empty() || nConsumersStillProcessingThisBatch == 0) {
        loadNextBatch();
    }

    if (_buffer.empty()) {
        // If we've loaded the next batch and it's still empty, then we've exhausted our input.
        return DocumentSource::GetNextResult::makeEOF();
    }

    if (_consumers[consumerId].nLeftToReturn == 0) {
        // This consumer has reached the end of this batch, but there are still other consumers that
        // haven't seen this whole batch.
        return DocumentSource::GetNextResult::makePauseExecution();
    }

    const size_t bufferIndex = _buffer.size() - _consumers[consumerId].nLeftToReturn;
    --_consumers[consumerId].nLeftToReturn;

    return _buffer[bufferIndex];
}

void TeeBuffer::loadNextBatch() {
    _buffer.clear();
    size_t bytesInBuffer = 0;

    auto input = _source->getNext();
    for (; input.isAdvanced(); input = _source->getNext()) {
        bytesInBuffer += input.getDocument().getApproximateSize();
        _buffer.push_back(std::move(input));

        if (bytesInBuffer >= _bufferSizeBytes) {
            break;  // Need to break here so we don't get the next input and accidentally ignore it.
        }
    }

    // For the following reasons, we invariant that we never get a paused input:
    //   - TeeBuffer is the only place where a paused GetNextReturn will be returned.
    //   - The $facet stage is the only stage that uses TeeBuffer.
    //   - We currently disallow nested $facet stages.
    invariant(!input.isPaused());  // NOLINT(bugprone-use-after-move)

    // Populate the pending returns.
    for (size_t consumerId = 0; consumerId < _consumers.size(); ++consumerId) {
        if (_consumers[consumerId].stillInUse) {
            _consumers[consumerId].nLeftToReturn = _buffer.size();
        }
    }
}

}  // namespace mongo
