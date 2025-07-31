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

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/document_source_cursor.h"
#include "mongo/db/query/plan_summary_stats.h"

#include <cstddef>
#include <deque>
#include <memory>
#include <string>

#include <boost/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace exec {
namespace agg {

/**
 * Constructs and returns Documents from the BSONObj objects produced by a supplied PlanExecutor.
 */
class CursorStage : public Stage {
public:
    using CursorType = DocumentSourceCursor::CursorType;
    using ResumeTrackingType = DocumentSourceCursor::ResumeTrackingType;
    using CatalogResourceHandle = DocumentSourceCursor::CatalogResourceHandle;

    CursorStage(StringData stageName,
                const boost::intrusive_ptr<CatalogResourceHandle>& catalogResourceHandle,
                const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                CursorType cursorType,
                ResumeTrackingType resumeTrackingType,
                std::shared_ptr<CursorSharedState> sharedState);

    ~CursorStage() override;

    Timestamp getLatestOplogTimestamp() const {
        return _latestOplogTimestamp;
    }

    /**
     * Returns a postBatchResumeToken compatible with resharding oplog sync, if available.
     * Otherwise, returns an empty object.
     */
    BSONObj getPostBatchResumeToken() const;

    const std::string& getPlanSummaryStr() const {
        return _planSummary;
    }

    const PlanSummaryStats& getPlanSummaryStats() const {
        return _sharedState->stats.planSummaryStats;
    }

    bool usedDisk() const final {
        return _sharedState->stats.planSummaryStats.usedDisk;
    }

    const SpecificStats* getSpecificStats() const final {
        return &_sharedState->stats;
    }

    void detachFromOperationContext() final;

    void reattachToOperationContext(OperationContext* opCtx) final;

protected:
    /**
     * This function hook is called on each 'obj' returned by '_sharedState->exec' when loading a
     * batch and returns a Document to be added to '_currentBatch'.
     *
     * The default implementation is the identity function.
     */
    virtual Document transformDoc(Document&& doc) const {
        return std::move(doc);
    }

private:
    GetNextResult doGetNext() final;

    void doForceSpill() final;

    /**
     * A $cursor stage loads documents from the underlying PlanExecutor in batches. An object of
     * this class represents one such batch. Acts like a queue into which documents can be queued
     * and dequeued in FIFO order.
     */
    class Batch {
    public:
        Batch(CursorType type) : _type(type) {}

        /**
         * Adds a new document to the batch.
         * The resume token is used to track the resume token for this document for non-oplog
         * queries.
         */
        void enqueue(Document&& doc, boost::optional<BSONObj> resumeToken);

        /**
         * Removes the first document from the batch.
         */
        Document dequeue();

        void clear();

        bool isEmpty() const;

        /**
         * Returns the approximate memory footprint of this batch, measured in bytes. Even after
         * documents are dequeued from the batch, continues to indicate the batch's peak memory
         * footprint. Resets to zero once the final document in the batch is dequeued.
         */
        size_t memUsageBytes() const {
            return _memUsageBytes;
        }

        /**
         * Illegal to call unless the CursorType is 'kRegular'.
         */
        const Document& peekFront() const {
            invariant(_type == CursorType::kRegular);
            return _batchOfDocs.front();
        }

        const BSONObj& peekFrontResumeToken() const {
            invariant(_type == CursorType::kRegular);
            return _resumeTokens.front();
        }

        /**
         * Returns the number of documents currently in the batch.
         */
        size_t count() const {
            return _type == CursorType::kRegular ? _batchOfDocs.size() : _count;
        }

    private:
        // If 'kEmptyDocuments', then dependency analysis has indicated that all we need to execute
        // the query is a count of the incoming documents.
        const CursorType _type;

        // Used only if '_type' is 'kRegular'. A deque of the documents comprising the batch.
        std::deque<Document> _batchOfDocs;

        // Used only if '_type' is 'kRegular' and this is a resumable query for a non-oplog
        // collection
        std::deque<BSONObj> _resumeTokens;

        // Used only if '_type' is 'kEmptyDocuments'. In this case, we don't need to keep the
        // documents themselves, only a count of the number of documents in the batch.
        size_t _count = 0;

        // The approximate memory footprint of the batch in bytes. Always kept at zero when '_type'
        // is 'kEmptyDocuments'.
        size_t _memUsageBytes = 0;
    };

    /**
     * Disposes of '_sharedState->exec' if it hasn't been disposed already. This involves taking a
     * collection lock.
     */
    void doDispose() final;

    /**
     * Acquires the appropriate locks, then destroys and de-registers '_sharedState->exec'.
     * '_sharedState->exec' must be non-null.
     */
    void cleanupExecutor();

    /**
     * Reads a batch of data from '_sharedState->exec'. Subclasses can specify custom behavior to be
     * performed on each document by overloading transformDoc().
     */
    void loadBatch();

    /**
     * Records stats about the plan used by '_sharedState->exec' into '_sharedState->stats'.
     */
    void recordPlanSummaryStats();

    /**
     * If we are tailing the oplog, this method updates the cached timestamp to that of the latest
     * document returned, or the latest timestamp observed in the oplog if we have no more results.
     */
    void _updateOplogTimestamp();

    /**
     * If we are tracking resume tokens for non-oplog scans, this method updates our cached resume
     * token.
     */
    void _updateNonOplogResumeToken();

    /**
     * Initialize the exponential growth batch size which allows for batching a small number of
     * documents when no $limit is pushed down into underlying executor. This approach can offer a
     * performance benefit when only a limited amount of data is required. However, small batching
     * may necessitate multiple yields in a potentially fast query, that's why we avoid to do so
     * when $limit is pushed down. Note that we still maintain a separate size limit in bytes
     * controlled by 'internalDocumentSourceCursorBatchSizeBytes' parameter.
     */
    void initializeBatchSizeCounts();

    /**
     * Helper function that reads data out of the underlying executor and into the current
     * batch. Returns a boolean indicating whether the executor can be destroyed.
     */
    bool pullDataFromExecutor(OperationContext* opCtx);

    // Batches results returned from the underlying PlanExecutor.
    Batch _currentBatch;

    // Handle on catalog state that can be acquired and released during loadBatch().
    boost::intrusive_ptr<CatalogResourceHandle> _catalogResourceHandle;

    // Whether we are tracking the latest observed oplog timestamp, the resume token from the
    // (non-oplog) scan, or neither.
    ResumeTrackingType _resumeTrackingType;

    std::string _planSummary;

    // If we are tracking the latest observed oplog time, this is the latest timestamp seen in the
    // oplog. Otherwise, this is a null timestamp.
    Timestamp _latestOplogTimestamp;

    // If we are tracking a non-oplog resume token, the resume token for the last document we
    // returned, or the current resume token at EOF.
    BSONObj _latestNonOplogResumeToken;

    PlanExecutor::QueryFramework _queryFramework;

    // The size of each batch, grows exponentially. 0 means unlimited.
    size_t _batchSizeCount = 0;
    // The size limit in bytes of each batch.
    size_t _batchSizeBytes = 0;

    const std::shared_ptr<CursorSharedState> _sharedState;
};

}  // namespace agg
}  // namespace exec
}  // namespace mongo
