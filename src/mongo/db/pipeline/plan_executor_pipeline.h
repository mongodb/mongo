/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/agg/exec_pipeline.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/explain_util.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/plan_explainer_pipeline.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_explainer.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/query/restore_context.h"
#include "mongo/db/query/write_ops/update_result.h"
#include "mongo/db/record_id.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"

#include <memory>
#include <queue>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

/**
 * A plan executor which is used to execute a Pipeline of DocumentSources.
 */
class PlanExecutorPipeline final : public PlanExecutor {
public:
    /**
     * Determines the type of resumable scan being run by the PlanExecutorPipeline.
     */
    enum class ResumableScanType {
        kNone,              // No resuming. This is the default.
        kChangeStream,      // For change stream pipelines.
        kNaturalOrderScan,  // For pipelines requesting a record ID resume token from a natural
                            // order non-oplog scan.
        kOplogScan          // For non-changestream resumable oplog scans.
    };

    PlanExecutorPipeline(boost::intrusive_ptr<ExpressionContext> expCtx,
                         std::unique_ptr<Pipeline> pipeline,
                         ResumableScanType resumableScanType);

    CanonicalQuery* getCanonicalQuery() const override {
        return nullptr;
    }

    const NamespaceString& nss() const override {
        return _expCtx->getNamespaceString();
    }

    const std::vector<NamespaceStringOrUUID>& getSecondaryNamespaces() const final {
        // Return a reference to an empty static array. This array will never contain any elements
        // because even though a PlanExecutorPipeline can reference multiple collections, it never
        // takes any locks over said namespaces (this is the responsibility of DocumentSources
        // which internally manage their own PlanExecutors).
        const static std::vector<NamespaceStringOrUUID> emptyNssVector;
        return emptyNssVector;
    }

    OperationContext* getOpCtx() const override {
        return _expCtx->getOperationContext();
    }

    // Pipeline execution does not support the saveState()/restoreState() interface. Instead, the
    // underlying data access plan is saved/restored internally in between DocumentSourceCursor
    // batches, or when the underlying PlanStage tree yields.
    void saveState() override {}
    void restoreState(const RestoreContext&) override {}

    void detachFromOperationContext() override {
        _execPipeline->detachFromOperationContext();
        _pipeline->detachFromOperationContext();
    }

    void reattachToOperationContext(OperationContext* opCtx) override {
        _execPipeline->reattachToOperationContext(opCtx);
        _pipeline->reattachToOperationContext(opCtx);
    }

    ExecState getNext(BSONObj* objOut, RecordId* recordIdOut) override;
    ExecState getNextDocument(Document& docOut) override;

    bool isEOF() const override;

    // DocumentSource execution is only used for executing aggregation commands, so the interfaces
    // for executing other CRUD operations are not supported.
    long long executeCount() override {
        MONGO_UNREACHABLE;
    }
    UpdateResult getUpdateResult() const override {
        MONGO_UNREACHABLE;
    }
    long long getDeleteResult() const override {
        MONGO_UNREACHABLE;
    }
    BatchedDeleteStats getBatchedDeleteStats() override {
        MONGO_UNREACHABLE;
    }

    void dispose(OperationContext* opCtx) override {
        _execPipeline->reattachToOperationContext(opCtx);
        _execPipeline->dispose();
    }

    void forceSpill(PlanYieldPolicy* yieldPolicy) override {
        tassert(10450600,
                "Pipelines acquire locks internally, so yieldPolicy must be nullptr",
                yieldPolicy == nullptr);
        _execPipeline->forceSpill();
    }

    void stashResult(const BSONObj& obj) override {
        _stash.push(obj.getOwned());
    }

    void markAsKilled(Status killStatus) override;

    bool isMarkedAsKilled() const override {
        return !_killStatus.isOK();
    }

    Status getKillStatus() const override {
        invariant(isMarkedAsKilled());
        return _killStatus;
    }

    bool isDisposed() const override {
        return _execPipeline->isDisposed();
    }

    Timestamp getLatestOplogTimestamp() const override {
        return _latestOplogTimestamp;
    }

    BSONObj getPostBatchResumeToken() const override {
        return _postBatchResumeToken;
    }

    LockPolicy lockPolicy() const override {
        return LockPolicy::kLocksInternally;
    }

    const PlanExplainer& getPlanExplainer() const final {
        return _planExplainer;
    }

    /**
     * Writes the explain information about the underlying pipeline to a std::vector<Value>,
     * providing the level of detail specified by 'verbosity'.
     */
    std::vector<Value> writeExplainOps(ExplainOptions::Verbosity verbosity) const {
        auto opts = SerializationOptions{.verbosity = verbosity};
        return (verbosity >= ExplainOptions::Verbosity::kExecStats)
            ? mergeExplains(*_pipeline, *_execPipeline, opts)
            : _pipeline->writeExplainOps(opts);
    }

    boost::optional<StringData> getExecutorType() const override {
        tassert(6253504, "Can't get type string without pipeline", _pipeline);
        return _pipeline->getTypeString();
    }

    PlanExecutor::QueryFramework getQueryFramework() const final;

    bool usesCollectionAcquisitions() const final {
        return true;
    }

private:
    /**
     * Obtains the next document from the underlying Pipeline, and does change streams-related
     * accounting if needed.
     */
    boost::optional<Document> _getNext();

    /**
     * Obtains the next result from the pipeline, gracefully handling any known exceptions which may
     * be thrown.
     */
    boost::optional<Document> _tryGetNext();

    /**
     * Serialize the given document to BSON while updating stats for BSONObjectTooLarge exception.
     */
    BSONObj _trySerializeToBson(const Document& doc);

    /**
     * For a change stream or resumable oplog scan, updates the scan state based on the latest
     * document returned by the underlying pipeline.
     */
    void _updateResumableScanState(const boost::optional<Document>& document);

    /**
     * If this is a change stream, advance the cluster time and post batch resume token based on the
     * latest document returned by the underlying pipeline.
     */
    void _performChangeStreamsAccounting(const boost::optional<Document>&);

    /**
     * Verifies that the docs's resume token has not been modified.
     */
    void _validateChangeStreamsResumeToken(const Document& event) const;

    /**
     * For a non-changestream resumable oplog scan, updates the latest oplog timestamp and
     * postBatchResumeToken value from the underlying pipeline.
     */
    void _performResumableOplogScanAccounting();

    /**
     * For a resumable natural order non-oplog scan, updates the postBatchResumeToken value from the
     * underlying pipeline.
     */
    void _performResumableNaturalOrderScanAccounting();

    /**
     * Set the speculative majority read timestamp if we have scanned up to a certain oplog
     * timestamp.
     */
    void _setSpeculativeReadTimestamp();

    /**
     * For a change stream or resumable oplog scan, initializes the scan state.
     */
    void _initializeResumableScanState();

    boost::intrusive_ptr<ExpressionContext> _expCtx;

    std::unique_ptr<Pipeline> _pipeline;
    std::unique_ptr<exec::agg::Pipeline> _execPipeline;

    PlanExplainerPipeline _planExplainer;

    std::queue<BSONObj> _stash;

    // If _killStatus has a non-OK value, then we have been killed and the value represents the
    // reason for the kill.
    Status _killStatus = Status::OK();

    // Set to true once we have received all results from the underlying '_pipeline', and the
    // pipeline has indicated end-of-stream.
    bool _pipelineIsEof = false;

    const ResumableScanType _resumableScanType{ResumableScanType::kNone};

    // If '_pipeline' is a change stream or other resumable scan type, these track the latest
    // timestamp seen while scanning the oplog, as well as the most recent PBRT.
    Timestamp _latestOplogTimestamp;
    BSONObj _postBatchResumeToken;
};

}  // namespace mongo
