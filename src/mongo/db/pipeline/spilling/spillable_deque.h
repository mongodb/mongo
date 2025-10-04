/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/memory_tracking/memory_usage_tracker.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/spilling/spilling_stats.h"
#include "mongo/db/storage/spill_table.h"

#include <cstddef>
#include <deque>
#include <memory>

namespace mongo {

/**
 * A deque like structure that spills to disk if the contents of the deque are larger than the given
 * maximum memory. Contents on disk are in a temporary table that will be cleaned up on startup if
 * the server crashes with data still present. Ids are not indexes into the deque but rather the
 * document number of the Documents inserted into the cache.
 */
class SpillableDeque {
public:
    SpillableDeque(ExpressionContext* expCtx, MemoryUsageTracker* tracker)
        : _memTracker((*tracker)["SpillableDeque"]), _expCtx(expCtx) {}

    /**
     * Adds 'input' to the in-memory cache and spills to disk if the document size puts us over the
     * memory limit and spilling is allowed.
     *
     * Note that the reported approximate size of 'input' may include the internal Document field
     * cache along with the underlying BSON size, which can change depending on the access pattern.
     * This class assumes that the size of the Document does not change from the time that it's
     * added here until it's freed via freeUpTo().
     */
    void addDocument(Document input);

    /**
     * Access a specific document. Calling 'getDocumentById' on a document not in the cache will
     * throw.
     */
    bool isIdInCache(int id);
    Document getDocumentById(int id);

    Document peekFront() {
        return getDocumentById(getLowestIndex());
    }

    void popFront() {
        freeUpTo(getLowestIndex());
    }

    /**
     * Removes all documents with ids up to but not including 'id' from the cache.
     */
    void freeUpTo(int id);

    /**
     * Remove all documents from the cache and reset state while preserving the ability to perform
     * more inserts.
     */
    void clear();

    /**
     * Destroy the SpillableDeque. No other functions should be called after finalize.
     * This function acquires a lock and can throw, and therefore should not be called in a
     * destructor (or any environment where it is not safe to throw). If this is not called before
     * the SpillableDeque is destructed the temporary table will eventually be cleaned up by the
     * storage engine.
     */
    void finalize() {
        if (_diskCache) {
            updateStorageSizeStat();
            _diskCache = nullptr;
        }
        _memCache.clear();
    }

    size_t getApproximateSize() const {
        return _memTracker.inUseTrackedMemoryBytes();
    }

    int getNumDocs() const {
        if (_diskWrittenIndex == 0 || _nextFreedIndex > _diskWrittenIndex) {
            return _memCache.size();
        }
        return _memCache.size() + _diskWrittenIndex - _nextFreedIndex;
    }

    bool empty() const {
        return getNumDocs() == 0;
    }

    bool usedDisk() const {
        return _stats.getSpills() > 0;
    }

    /**
     * Returns the id of the last document inserted.
     */
    int getHighestIndex() const {
        return _nextIndex - 1;
    }
    /**
     * Returns the lowest id in the cache.
     */
    int getLowestIndex() const {
        return _nextFreedIndex;
    }

    /**
     * Spills all documents currently in memory to disk. Throws if '_expCtx->getAllowDiskUse()' is
     *false. This is also called automatically as part of 'addDocument', but call this to spill
     *without adding.
     */
    void spillToDisk();

    const SpillingStats& getSpillingStats() const {
        return _stats;
    }

private:
    Document readDocumentFromDiskById(int desired);
    Document readDocumentFromMemCacheById(int desired);
    void verifyInCache(int desired);

    void updateStorageSizeStat();

    // The pipeline stage using this class owns this memory tracker; this class only references it.
    SimpleMemoryUsageTracker& _memTracker;

    ExpressionContext* _expCtx;
    std::deque<MemoryUsageTokenWith<Document>> _memCache;

    std::unique_ptr<SpillTable> _diskCache = nullptr;
    // The number of documents we've written to disk, as well as the recordID of the last document
    // written. Zero is an invalid RecordID, so writing will start with RecordId(1).
    int _diskWrittenIndex = 0;
    // The next index to be released. Zero refers to the first document added to the cache.
    int _nextFreedIndex = 0;
    // The id of the next document to be added. Zero refers to the first document added to the
    // cache.
    int _nextIndex = 0;

    // When spilling to disk, only write batches smaller than 16MB.
    static constexpr size_t kMaxWriteSize = 16 * 1024 * 1024;

    SpillingStats _stats;
};

}  // namespace mongo
