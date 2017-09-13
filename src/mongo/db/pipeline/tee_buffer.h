/**
 *    Copyright (C) 2016 MongoDB, Inc.
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

#pragma once

#include <algorithm>
#include <boost/intrusive_ptr.hpp>
#include <vector>

#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/query/query_knobs.h"
#include "mongo/util/intrusive_counter.h"

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
        size_t nConsumers, int bufferSizeBytes = internalQueryFacetBufferSizeBytes.load());

    void setSource(DocumentSource* source) {
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

    DocumentSource* _source = nullptr;

    const size_t _bufferSizeBytes;
    std::vector<DocumentSource::GetNextResult> _buffer;

    struct ConsumerInfo {
        bool stillInUse = true;
        int nLeftToReturn = 0;
    };
    std::vector<ConsumerInfo> _consumers;
};
}  // namespace mongo
