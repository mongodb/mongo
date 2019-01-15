
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

#include "mongo/platform/basic.h"

#include "mongo/s/query/router_stage_pipeline.h"

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/document_source_list_local_sessions.h"
#include "mongo/db/pipeline/document_source_merge_cursors.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/s/query/document_source_router_adapter.h"

namespace mongo {

RouterStagePipeline::RouterStagePipeline(std::unique_ptr<RouterExecStage> child,
                                         std::unique_ptr<Pipeline, PipelineDeleter> mergePipeline)
    : RouterExecStage(mergePipeline->getContext()->opCtx),
      _mergePipeline(std::move(mergePipeline)),
      _mongosOnlyPipeline(!_mergePipeline->isSplitForMerge()) {
    if (!_mongosOnlyPipeline) {
        // Save a pointer to the child RouterExecStage before it is absorbed into the pipeline. This
        // is either a merge stage, or an ancestor that can forward calls to the RouterStageMerge.
        _mergeCursorsStage = child.get();
        // Add an adapter to the front of the pipeline to draw results from 'child'.
        _routerAdapter =
            DocumentSourceRouterAdapter::create(_mergePipeline->getContext(), std::move(child)),
        _mergePipeline->addInitialSource(_routerAdapter);
    }
}

StatusWith<ClusterQueryResult> RouterStagePipeline::next(RouterExecStage::ExecContext execContext) {
    if (_routerAdapter) {
        _routerAdapter->setExecContext(execContext);
    }

    // Pipeline::getNext will return a boost::optional<Document> or boost::none if EOF.
    if (auto result = _mergePipeline->getNext()) {
        _validateAndRecordSortKey(*result);
        return {result->toBson()};
    }

    // If we reach this point, we have hit EOF.
    if (!_mergePipeline->getContext()->isTailableAwaitData()) {
        _mergePipeline.get_deleter().dismissDisposal();
        _mergePipeline->dispose(getOpCtx());
    }

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

std::size_t RouterStagePipeline::getNumRemotes() const {
    return _mongosOnlyPipeline ? 0 : _routerAdapter->getNumRemotes();
}

BSONObj RouterStagePipeline::getPostBatchResumeToken() {
    auto pbrt = _mergeCursorsStage ? _mergeCursorsStage->getPostBatchResumeToken() : BSONObj();
    return pbrt.isEmpty() ? pbrt : _setPostBatchResumeTokenUUID(pbrt);
}

BSONObj RouterStagePipeline::_setPostBatchResumeTokenUUID(BSONObj pbrt) const {
    // If the PBRT does not match the sort key of the latest document, it is a high water mark.
    static const auto tokenFormat = ResumeToken::SerializationFormat::kHexString;
    const bool isHighWaterMark = !pbrt.binaryEqual(_latestSortKey);

    // If this stream is on a single collection and the token is a high water mark, then it may have
    // come from a shard that does not have the collection. If so, we must fill in the correct UUID.
    if (isHighWaterMark && _mergePipeline->getContext()->uuid) {
        auto tokenData = ResumeToken::parse(pbrt).getData();
        // Check whether the UUID is missing before regenerating the token.
        if (!tokenData.uuid) {
            invariant(tokenData.tokenType == ResumeTokenData::kHighWaterMarkToken);
            tokenData.uuid = _mergePipeline->getContext()->uuid;
            pbrt = ResumeToken(tokenData).toDocument(tokenFormat).toBson();
        }
    }
    return pbrt;
}

void RouterStagePipeline::_validateAndRecordSortKey(const Document& doc) {
    _latestSortKey = doc.getSortKeyMetaField();
}

bool RouterStagePipeline::remotesExhausted() {
    return _mongosOnlyPipeline || _routerAdapter->remotesExhausted();
}

Status RouterStagePipeline::doSetAwaitDataTimeout(Milliseconds awaitDataTimeout) {
    return _routerAdapter->setAwaitDataTimeout(awaitDataTimeout);
}

}  // namespace mongo
