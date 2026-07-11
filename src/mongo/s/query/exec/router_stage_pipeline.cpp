// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/query/exec/router_stage_pipeline.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/agg/pipeline_builder.h"
#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/s/query/exec/merge_cursors_stage.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <utility>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

RouterStagePipeline::RouterStagePipeline(std::unique_ptr<Pipeline> mergePipeline)
    : RouterExecStage(mergePipeline->getContext()->getOperationContext()),
      _mergePipeline(std::move(mergePipeline)),
      _mergeExecPipeline(exec::agg::buildPipeline(_mergePipeline->freeze())) {
    tassert(11052348, "Merge pipeline cannot be empty", !_mergePipeline->getSources().empty());
    _mergeCursorsStage =
        dynamic_cast<exec::agg::MergeCursorsStage*>(_mergeExecPipeline->getStages().front().get());

    // If there is an initial post batch resume token, set it on the merge cursors stage.
    if (auto pbrt = _mergePipeline->getContext()->getInitialPostBatchResumeToken();
        !pbrt.isEmpty() && _mergeCursorsStage) {
        _mergeCursorsStage->setHighWaterMark(pbrt.getOwned());
    }
}

StatusWith<ClusterQueryResult> RouterStagePipeline::next() {
    // Pipeline::getNext will return a boost::optional<Document> or boost::none if EOF.
    if (auto result = _mergeExecPipeline->getNext()) {
        return _validateAndConvertToBSON(*result);
    }

    // If we reach this point, we have hit EOF.
    if (!_mergePipeline->getContext()->isTailableAwaitData()) {
        _mergeExecPipeline->dispose();
    }

    return {ClusterQueryResult()};
}

void RouterStagePipeline::doReattachToOperationContext() {
    _mergeExecPipeline->reattachToOperationContext(getOpCtx());
    _mergePipeline->reattachToOperationContext(getOpCtx());
}

void RouterStagePipeline::doDetachFromOperationContext() {
    _mergeExecPipeline->detachFromOperationContext();
    _mergePipeline->detachFromOperationContext();
}

void RouterStagePipeline::kill(OperationContext* opCtx) {
    _mergeExecPipeline->reattachToOperationContext(opCtx);
    _mergeExecPipeline->dispose();
}

std::size_t RouterStagePipeline::getNumRemotes() const {
    if (_mergeCursorsStage) {
        return _mergeCursorsStage->getNumRemotes();
    }
    return 0;
}

BSONObj RouterStagePipeline::getPostBatchResumeToken() {
    return _mergeCursorsStage ? _mergeCursorsStage->getHighWaterMark() : BSONObj();
}

BSONObj RouterStagePipeline::_validateAndConvertToBSON(const Document& event) {
    // If this is not a change stream pipeline, we have nothing to do except return the BSONObj.
    if (!_mergePipeline->getContext()->isTailableAwaitData()) {
        return event.toBson();
    }
    // Confirm that the document _id field matches the original resume token in the sort key field.
    auto eventBSON = event.toBson();
    auto resumeToken = event.metadata().getSortKey();
    auto idField = eventBSON.getObjectField("_id");
    tassert(11052349, "Resume token is missing from event", !resumeToken.missing());
    uassert(ErrorCodes::ChangeStreamFatalError,
            str::stream() << "Encountered an event whose _id field, which contains the resume "
                             "token, was modified by the pipeline. Modifying the _id field of an "
                             "event makes it impossible to resume the stream from that point. Only "
                             "transformations that retain the unmodified _id field are allowed. "
                             "Expected: "
                          << BSON("_id" << resumeToken) << " but found: "
                          << (eventBSON["_id"] ? BSON("_id" << eventBSON["_id"]) : BSONObj()),
            (resumeToken.getType() == BSONType::object) &&
                idField.binaryEqual(resumeToken.getDocument().toBson()));

    // Return the event in BSONObj form, minus the $sortKey metadata.
    return eventBSON;
}

bool RouterStagePipeline::remotesExhausted() const {
    // Change stream pipelines can never be exhausted. Instead invalidation event may be sent,
    // closing the stream.
    if (_mergePipeline->getContext()->isTailableAwaitData()) {
        return false;
    }

    return !_mergeCursorsStage || _mergeCursorsStage->remotesExhausted();
}

Status RouterStagePipeline::doSetAwaitDataTimeout(Milliseconds awaitDataTimeout) {
    tassert(11052350,
            "The only cursors which should be tailable are those with remote cursors.",
            _mergeCursorsStage);
    return _mergeCursorsStage->setAwaitDataTimeout(awaitDataTimeout);
}

}  // namespace mongo
