/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/s/query/router_stage_pipeline.h"

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/document_source_list_local_sessions.h"
#include "mongo/db/pipeline/document_source_merge_cursors.h"
#include "mongo/db/pipeline/expression_context.h"

namespace mongo {

namespace {

/**
 * A class that acts as an adapter between the RouterExecStage and DocumentSource interfaces,
 * translating results from an input RouterExecStage into DocumentSource::GetNextResults.
 */
class DocumentSourceRouterAdapter : public DocumentSource {
public:
    static boost::intrusive_ptr<DocumentSourceRouterAdapter> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        std::unique_ptr<RouterExecStage> childStage) {
        return new DocumentSourceRouterAdapter(expCtx, std::move(childStage));
    }

    GetNextResult getNext() final {
        auto next = uassertStatusOK(_child->next());
        if (auto nextObj = next.getResult()) {
            return Document::fromBsonWithMetaData(*nextObj);
        }
        return GetNextResult::makeEOF();
    }

    void doDispose() final {
        _child->kill(pExpCtx->opCtx);
    }

    void reattachToOperationContext(OperationContext* opCtx) final {
        _child->reattachToOperationContext(opCtx);
    }

    void detachFromOperationContext() final {
        _child->detachFromOperationContext();
    }

    Value serialize(boost::optional<ExplainOptions::Verbosity> explain) const final {
        invariant(explain);  // We shouldn't need to serialize this stage to send it anywhere.
        return Value();      // Return the empty value to hide this stage from explain output.
    }

    bool remotesExhausted() {
        return _child->remotesExhausted();
    }

private:
    DocumentSourceRouterAdapter(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                std::unique_ptr<RouterExecStage> childStage)
        : DocumentSource(expCtx), _child(std::move(childStage)) {}

    std::unique_ptr<RouterExecStage> _child;
};
}  // namespace

RouterStagePipeline::RouterStagePipeline(std::unique_ptr<RouterExecStage> child,
                                         std::unique_ptr<Pipeline, Pipeline::Deleter> mergePipeline)
    : RouterExecStage(mergePipeline->getContext()->opCtx),
      _mergePipeline(std::move(mergePipeline)),
      _mongosOnly(!_mergePipeline->allowedToForwardFromMongos()) {
    if (_mongosOnly) {
        invariant(_mergePipeline->canRunOnMongos());
    } else {
        // Add an adapter to the front of the pipeline to draw results from 'child'.
        _mergePipeline->addInitialSource(
            DocumentSourceRouterAdapter::create(_mergePipeline->getContext(), std::move(child)));
    }
}

StatusWith<ClusterQueryResult> RouterStagePipeline::next() {
    // Pipeline::getNext will return a boost::optional<Document> or boost::none if EOF.
    if (auto result = _mergePipeline->getNext()) {
        return {result->toBson()};
    }

    // If we reach this point, we have hit EOF.
    _mergePipeline.get_deleter().dismissDisposal();
    _mergePipeline->dispose(getOpCtx());

    return {ClusterQueryResult()};
}

void RouterStagePipeline::doReattachToOperationContext() {
    _mergePipeline->reattachToOperationContext(getOpCtx());
}

void RouterStagePipeline::doDetachFromOperationContext() {
    _mergePipeline->detachFromOperationContext();
}

void RouterStagePipeline::kill(OperationContext* opCtx) {
    _mergePipeline.get_deleter().dismissDisposal();
    _mergePipeline->dispose(opCtx);
}

bool RouterStagePipeline::remotesExhausted() {
    return _mongosOnly ||
        static_cast<DocumentSourceRouterAdapter*>(_mergePipeline->getSources().front().get())
            ->remotesExhausted();
}

Status RouterStagePipeline::doSetAwaitDataTimeout(Milliseconds awaitDataTimeout) {
    return {ErrorCodes::InvalidOptions, "maxTimeMS is not valid for aggregation getMore"};
}

}  // namespace mongo
