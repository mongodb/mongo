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

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <cstddef>
#include <deque>
#include <memory>
#include <set>
#include <string>
#include <utility>

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_explainer.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/assert_util.h"

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
     * Indicates whether we are tracking resume information from an oplog query (e.g. for
     * change streams), from a non-oplog query (natural order scan using recordId information)
     * or neither.
     */
    enum class ResumeTrackingType { kNone, kOplog, kNonOplog };

    /**
     * Create a document source based on a passed-in PlanExecutor. 'exec' must be a yielding
     * PlanExecutor, and must be registered with the associated collection's CursorManager.
     *
     * If 'cursorType' is 'kEmptyDocuments', then we inform the $cursor stage that this is a count
     * scenario -- the dependency set is fully known and is empty. In this case, the newly created
     * $cursor stage can return a sequence of empty documents for the caller to count.
     */
    static boost::intrusive_ptr<DocumentSourceCursor> create(
        const MultipleCollectionAccessor& collections,
        std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec,
        const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
        CursorType cursorType,
        ResumeTrackingType resumeTrackingType = ResumeTrackingType::kNone);

    const char* getSourceName() const override;

    static const Id& id;

    Id getId() const override {
        return id;
    }

    Value serialize(const SerializationOptions& opts = SerializationOptions{}) const final;

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

    /**
     * Returns a postBatchResumeToken compatible with resharding oplog sync, if available.
     * Otherwise, returns an empty object.
     */
    BSONObj getPostBatchResumeToken() const;

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

    PlanExecutor::QueryFramework getQueryFramework() const {
        return _queryFramework;
    }

    BSONObj serializeToBSONForDebug() const final {
        // Feel free to add any useful information here. For now this has not been useful for
        // debugging so is left empty.
        return BSON(kStageName << "{}");
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const final {
        // The assumption is that dependency analysis and non-correlated prefix analysis happens
        // before a $cursor is attached to a pipeline.
        MONGO_UNREACHABLE;
    }

protected:
    DocumentSourceCursor(const MultipleCollectionAccessor& collections,
                         std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec,
                         const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                         CursorType cursorType,
                         ResumeTrackingType resumeTrackingType = ResumeTrackingType::kNone);

    GetNextResult doGetNext() final;

    ~DocumentSourceCursor() override;

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

    // Whether we are tracking the latest observed oplog timestamp, the resume token from the
    // (non-oplog) scan, or neither.
    ResumeTrackingType _resumeTrackingType = ResumeTrackingType::kNone;

    // If we are tracking the latest observed oplog time, this is the latest timestamp seen in the
    // oplog. Otherwise, this is a null timestamp.
    Timestamp _latestOplogTimestamp;

    // If we are tracking a non-oplog resume token, the resume token for the last document we
    // returned, or the current resume token at EOF.
    BSONObj _latestNonOplogResumeToken;

    // Specific stats for $cursor stage.
    DocumentSourceCursorStats _stats;

    PlanExecutor::QueryFramework _queryFramework;

    // The size of each batch, grows exponentially. 0 means unlimited.
    size_t _batchSizeCount = 0;
    // The size limit in bytes of each batch.
    size_t _batchSizeBytes = 0;
};

}  // namespace mongo
