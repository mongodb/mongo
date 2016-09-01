/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/tee_buffer.h"

#include <algorithm>

#include "mongo/db/pipeline/document.h"

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
    invariant(!input.isPaused());

    // Populate the pending returns.
    for (size_t consumerId = 0; consumerId < _consumers.size(); ++consumerId) {
        if (_consumers[consumerId].stillInUse) {
            _consumers[consumerId].nLeftToReturn = _buffer.size();
        }
    }
}

}  // namespace mongo
