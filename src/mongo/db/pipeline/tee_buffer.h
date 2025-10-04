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

#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/query/stage_memory_limit_knobs/knobs.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/intrusive_counter.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <vector>

#include <boost/intrusive_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

/**
 * This stage takes a stream of input documents and makes them available to multiple consumers. To
 * do so, it will batch incoming documents and allow each consumer to consume one batch at a time.
 * As a consequence, consumers must be able to pause their execution to allow other consumers to
 * process the batch before moving to the next batch.
 */
class TeeBuffer : public RefCountable {
public:
    /**
     * Creates a TeeBuffer that will make results available to 'nConsumers' consumers. Note that
     * 'bufferSizeBytes' is a soft cap, and may be exceeded by one document's worth (~16MB).
     */
    static boost::intrusive_ptr<TeeBuffer> create(
        size_t nConsumers,
        int bufferSizeBytes = loadMemoryLimit(StageMemoryLimit::QueryFacetBufferSizeBytes));

    void setSource(exec::agg::Stage* source) {
        _source = source;
    }

    /**
     * Removes 'consumerId' as a consumer of this buffer. This is required to be called if a
     * consumer will not consume all input.
     */
    void dispose(size_t consumerId) {
        _consumers[consumerId].stillInUse = false;
        _consumers[consumerId].nLeftToReturn = 0;
        if (std::none_of(_consumers.begin(), _consumers.end(), [](const ConsumerInfo& info) {
                return info.stillInUse;
            })) {
            _buffer.clear();
            if (_source) {
                _source->dispose();
            }
        }
    }

    /**
     * Retrieves the next document meant to be consumed by the pipeline given by 'consumerId'.
     * Returns GetNextState::ResultState::kPauseExecution if this pipeline has consumed the whole
     * buffer, but other consumers are still using it.
     */
    DocumentSource::GetNextResult getNext(size_t consumerId);

private:
    TeeBuffer(size_t nConsumers, size_t bufferSizeBytes);

    /**
     * Clears '_buffer', then keeps requesting results from '_source' and pushing them all into
     * '_buffer', until more than '_bufferSizeBytes' of documents have been returned, or until
     * '_source' is exhausted.
     */
    void loadNextBatch();

    exec::agg::Stage* _source = nullptr;

    const size_t _bufferSizeBytes;
    std::vector<DocumentSource::GetNextResult> _buffer;

    struct ConsumerInfo {
        bool stillInUse = true;
        int nLeftToReturn = 0;
    };
    std::vector<ConsumerInfo> _consumers;
};
}  // namespace mongo
