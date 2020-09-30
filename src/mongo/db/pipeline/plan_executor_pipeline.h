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

#include <queue>

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/plan_explainer_pipeline.h"
#include "mongo/db/query/plan_executor.h"

namespace mongo {

/**
 * A plan executor which is used to execute a Pipeline of DocumentSources.
 */
class PlanExecutorPipeline final : public PlanExecutor {
public:
    PlanExecutorPipeline(boost::intrusive_ptr<ExpressionContext> expCtx,
                         std::unique_ptr<Pipeline, PipelineDeleter> pipeline,
                         bool isChangeStream);

    CanonicalQuery* getCanonicalQuery() const override {
        return nullptr;
    }

    const NamespaceString& nss() const override {
        return _expCtx->ns;
    }

    OperationContext* getOpCtx() const override {
        return _expCtx->opCtx;
    }

    // Pipeline execution does not support the saveState()/restoreState() interface. Instead, the
    // underlying data access plan is saved/restored internally in between DocumentSourceCursor
    // batches, or when the underlying PlanStage tree yields.
    void saveState() override {}
    void restoreState(const RestoreContext&) override {}

    void detachFromOperationContext() override {
        _pipeline->detachFromOperationContext();
    }

    void reattachToOperationContext(OperationContext* opCtx) override {
        _pipeline->reattachToOperationContext(opCtx);
    }

    ExecState getNext(BSONObj* objOut, RecordId* recordIdOut) override;
    ExecState getNextDocument(Document* docOut, RecordId* recordIdOut) override;

    bool isEOF() override;

    // DocumentSource execution is only used for executing aggregation commands, so the interfaces
    // for executing other CRUD operations are not supported.
    long long executeCount() override {
        MONGO_UNREACHABLE;
    }
    UpdateResult executeUpdate() override {
        MONGO_UNREACHABLE;
    }
    UpdateResult getUpdateResult() const override {
        MONGO_UNREACHABLE;
    }
    long long executeDelete() override {
        MONGO_UNREACHABLE;
    }

    void dispose(OperationContext* opCtx) override {
        _pipeline->dispose(opCtx);
    }

    void enqueue(const BSONObj& obj) override {
        _stash.push(obj.getOwned());
    }

    void markAsKilled(Status killStatus) override;

    bool isMarkedAsKilled() const override {
        return !_killStatus.isOK();
    }

    Status getKillStatus() override {
        invariant(isMarkedAsKilled());
        return _killStatus;
    }

    bool isDisposed() const override {
        return _pipeline->isDisposed();
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
        return _pipeline->writeExplainOps(verbosity);
    }

private:
    /**
     * Obtains the next document from the underlying Pipeline, and does change streams-related
     * accounting if needed.
     */
    boost::optional<Document> _getNext();

    /**
     * If this is a change stream, advance the cluster time and post batch resume token based on the
     * latest document returned by the underlying pipeline.
     */
    void _performChangeStreamsAccounting(const boost::optional<Document>);

    /**
     * Verifies that the docs's resume token has not been modified.
     */
    void _validateResumeToken(const Document& event) const;

    /**
     * Set the speculative majority read timestamp if we have scanned up to a certain oplog
     * timestamp.
     */
    void _setSpeculativeReadTimestamp();

    boost::intrusive_ptr<ExpressionContext> _expCtx;

    std::unique_ptr<Pipeline, PipelineDeleter> _pipeline;

    PlanExplainerPipeline _planExplainer;

    const bool _isChangeStream;

    std::queue<BSONObj> _stash;

    // If _killStatus has a non-OK value, then we have been killed and the value represents the
    // reason for the kill.
    Status _killStatus = Status::OK();

    // Set to true once we have received all results from the underlying '_pipeline', and the
    // pipeline has indicated end-of-stream.
    bool _pipelineIsEof = false;

    // If '_pipeline' is a change stream, these track the latest timestamp seen while scanning the
    // oplog, as well as the most recent PBRT.
    Timestamp _latestOplogTimestamp;
    BSONObj _postBatchResumeToken;
};

}  // namespace mongo
