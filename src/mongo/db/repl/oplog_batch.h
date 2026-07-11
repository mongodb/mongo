// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/repl/oplog_entry.h"
#include "mongo/util/modules.h"

#include <vector>

namespace mongo {
namespace repl {

/**
 * Stores a batch of oplog entries. This is immutable.
 * The batch can be either BSONObj or OplogEntry.
 */
template <class T>
class [[MONGO_MOD_PUBLIC]] OplogBatch {
public:
    OplogBatch() : _batch(), _byteSize(0) {}

    OplogBatch(std::vector<T> entries, size_t byteSize)
        : _batch(std::move(entries)), _byteSize(byteSize) {}

    OplogBatch(typename std::vector<T>::const_iterator begin,
               typename std::vector<T>::const_iterator end,
               size_t byteSize)
        : _batch(begin, end), _byteSize(byteSize) {}

    bool empty() const {
        return _batch.empty();
    }

    const T& front() const {
        invariant(!_batch.empty());
        return _batch.front();
    }

    const T& back() const {
        invariant(!_batch.empty());
        return _batch.back();
    }

    const std::vector<T>& getBatch() const {
        return _batch;
    }

    size_t count() const {
        return _batch.size();
    }

    std::size_t byteSize() {
        return _byteSize;
    }

    /**
     * Leaves this object in an unspecified state. Only assignment and destruction are valid.
     */
    std::vector<T> releaseBatch() {
        return std::move(_batch);
    }

    /**
     * Passes the term when the buffer is exhausted to a higher level in case the node has stepped
     * down and then stepped up again. See its caller for more context.
     */
    boost::optional<long long> termWhenExhausted() const {
        return _termWhenExhausted;
    }

    void setTermWhenExhausted(long long term) {
        invariant(empty());
        _termWhenExhausted = term;
    }

private:
    std::vector<T> _batch;
    std::size_t _byteSize;
    boost::optional<long long> _termWhenExhausted;
};

/**
 * Stores a batch of oplog entries for oplog application.
 */
class [[MONGO_MOD_PARENT_PRIVATE]] OplogApplierBatch : public OplogBatch<OplogEntry> {
public:
    OplogApplierBatch() : OplogBatch<OplogEntry>() {}

    OplogApplierBatch(std::vector<OplogEntry> entries, size_t bytesSize)
        : OplogBatch<OplogEntry>(std::move(entries), bytesSize) {}

    /**
     * A batch with this set indicates that the upstream stages of the pipeline are shutdown
     * and no more batches will be coming.
     *
     * This can only happen with empty batches.
     */
    bool mustShutdown() const {
        return _mustShutdown;
    }

    void setMustShutdownFlag() {
        invariant(empty());
        _mustShutdown = true;
    }

private:
    bool _mustShutdown = false;
};

using OplogWriterBatch = OplogBatch<BSONObj>;

}  // namespace repl
}  // namespace mongo
