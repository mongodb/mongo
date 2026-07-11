// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/query/stage_memory_limit_knobs/knobs.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/modules.h"

#include <algorithm>
#include <cstddef>
#include <vector>

#include <boost/intrusive_ptr.hpp>
#include <boost/optional.hpp>
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
        OperationContext* opCtx,
        size_t nConsumers,
        boost::optional<int> bufferSizeBytes = boost::none);

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
