/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/repl/oplog_entry.h"

namespace mongo {
namespace repl {

/**
 * Stores a batch of oplog entries. This is immutable.
 * The batch can be either BSONObj or OplogEntry.
 */
template <class T>
class OplogBatch {
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

private:
    std::vector<T> _batch;
    std::size_t _byteSize;
};

/**
 * Stores a batch of oplog entries for oplog application.
 */
class OplogApplierBatch : public OplogBatch<OplogEntry> {
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
    bool _mustShutdown = false;
    boost::optional<long long> _termWhenExhausted;
};

/**
 * Stores a batch of oplog entry BSON for oplog writer.
 */
class OplogWriterBatch : public OplogBatch<BSONObj> {
public:
    OplogWriterBatch() : OplogBatch<BSONObj>() {}

    OplogWriterBatch(std::vector<BSONObj> entries, size_t bytesSize)
        : OplogBatch<BSONObj>(std::move(entries), bytesSize) {}

    /**
     * A batch with this set indicates that the buffer is in drain mode and the batch must be empty.
     */
    bool exhausted() const {
        return _exhausted;
    }

    void setExhausted() {
        invariant(empty());
        _exhausted = true;
    }

private:
    bool _exhausted = false;
};

}  // namespace repl
}  // namespace mongo
