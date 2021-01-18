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

#include <deque>

#include "mongo/db/db_raii.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_summary_stats.h"

namespace mongo {

/**
 * Constructs and returns Documents from the BSONObj objects produced by a supplied PlanExecutor.
 */
class DocumentSourceCursor : public DocumentSource {
public:
    static constexpr StringData kStageName = "$cursor"_sd;

    /**
     * Indicates whether or not this is a count-like operation. If the operation is count-like, then
     * the cursor can produce empty documents since the subsequent stages need only the count of
     * these documents (not the actual data).
     */
    enum class CursorType { kRegular, kEmptyDocuments };

    /**
     * Create a document source based on a passed-in PlanExecutor. 'exec' must be a yielding
     * PlanExecutor, and must be registered with the associated collection's CursorManager.
     *
     * If 'cursorType' is 'kEmptyDocuments', then we inform the $cursor stage that this is a count
     * scenario -- the dependency set is fully known and is empty. In this case, the newly created
     * $cursor stage can return a sequence of empty documents for the caller to count.
     */
    static boost::intrusive_ptr<DocumentSourceCursor> create(
        const CollectionPtr& collection,
        std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec,
        const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
        CursorType cursorType,
        bool trackOplogTimestamp = false);

    const char* getSourceName() const override;

    Value serialize(boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final;

    StageConstraints constraints(Pipeline::SplitState pipeState) const final {
        StageConstraints constraints(StreamType::kStreaming,
                                     PositionRequirement::kFirst,
                                     HostTypeRequirement::kAnyShard,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kNotAllowed,
                                     TransactionRequirement::kAllowed,
                                     LookupRequirement::kAllowed,
                                     UnionRequirement::kAllowed);

        constraints.requiresInputDocSource = false;
        return constraints;
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final {
        return boost::none;
    }

    void detachFromOperationContext() final;

    void reattachToOperationContext(OperationContext* opCtx) final;

    Timestamp getLatestOplogTimestamp() const {
        return _latestOplogTimestamp;
    }

    const std::string& getPlanSummaryStr() const {
        return _planSummary;
    }

    const PlanSummaryStats& getPlanSummaryStats() const {
        return _stats.planSummaryStats;
    }

    bool usedDisk() final {
        return _stats.planSummaryStats.usedDisk;
    }

    const SpecificStats* getSpecificStats() const final {
        return &_stats;
    }

    const PlanExplainer::ExplainVersion& getExplainVersion() const {
        return _exec->getPlanExplainer().getVersion();
    }

protected:
    DocumentSourceCursor(const CollectionPtr& collection,
                         std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec,
                         const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                         CursorType cursorType,
                         bool trackOplogTimestamp = false);

    GetNextResult doGetNext() final;

    ~DocumentSourceCursor();

    /**
     * Disposes of '_exec' if it hasn't been disposed already. This involves taking a collection
     * lock.
     */
    void doDispose() final;

    /**
     * If '_shouldProduceEmptyDocs' is false, this function hook is called on each 'obj' returned by
     * '_exec' when loading a batch and returns a Document to be added to '_currentBatch'.
     *
     * The default implementation is the identity function.
     */
    virtual Document transformDoc(Document&& doc) const {
        return std::move(doc);
    }

private:
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
         */
        void enqueue(Document&& doc);

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

    private:
        // If 'kEmptyDocuments', then dependency analysis has indicated that all we need to execute
        // the query is a count of the incoming documents.
        const CursorType _type;

        // Used only if '_type' is 'kRegular'. A deque of the documents comprising the batch.
        std::deque<Document> _batchOfDocs;

        // Used only if '_type' is 'kEmptyDocuments'. In this case, we don't need to keep the
        // documents themselves, only a count of the number of documents in the batch.
        size_t _count = 0;

        // The approximate memory footprint of the batch in bytes. Always kept at zero when '_type'
        // is 'kEmptyDocuments'.
        size_t _memUsageBytes = 0;
    };

    /**
     * Acquires the appropriate locks, then destroys and de-registers '_exec'. '_exec' must be
     * non-null.
     */
    void cleanupExecutor();

    /**
     * Reads a batch of data from '_exec'. Subclasses can specify custom behavior to be performed on
     * each document by overloading transformBSONObjToDocument().
     */
    void loadBatch();

    void recordPlanSummaryStats();

    /**
     * If we are tailing the oplog, this method updates the cached timestamp to that of the latest
     * document returned, or the latest timestamp observed in the oplog if we have no more results.
     */
    void _updateOplogTimestamp();

    // Batches results returned from the underlying PlanExecutor.
    Batch _currentBatch;

    // The underlying query plan which feeds this pipeline. Must be destroyed while holding the
    // collection lock.
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> _exec;

    // Status of the underlying executor, _exec. Used for explain queries if _exec produces an
    // error. Since _exec may not finish running (if there is a limit, for example), we store OK as
    // the default.
    Status _execStatus = Status::OK();

    std::string _planSummary;

    // Used only for explain() queries. Stores the stats of the winning plan when a plan was
    // selected by the multi-planner. When the query is executed (with exec->executePlan()), it will
    // wipe out its own copy of the winning plan's statistics, so they need to be saved here.
    boost::optional<PlanExplainer::PlanStatsDetails> _winningPlanTrialStats;

    // True if we are tracking the latest observed oplog timestamp, false otherwise.
    bool _trackOplogTS = false;

    // If we are tailing the oplog and tracking the latest observed oplog time, this is the latest
    // timestamp seen in the collection. Otherwise, this is a null timestamp.
    Timestamp _latestOplogTimestamp;

    // Specific stats for $cursor stage.
    DocumentSourceCursorStats _stats;
};

}  // namespace mongo
