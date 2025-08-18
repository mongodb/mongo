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

#include "mongo/db/exec/agg/change_stream_check_invalidate_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/pipeline/change_stream_start_after_invalidate_info.h"
#include "mongo/db/pipeline/document_source_change_stream_check_invalidate.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

boost::intrusive_ptr<exec::agg::Stage> documentSourceChangeStreamCheckInvalidateToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    auto* changeStreamCheckInvalidateDS =
        dynamic_cast<DocumentSourceChangeStreamCheckInvalidate*>(documentSource.get());

    tassert(10561302,
            "expected 'DocumentSourceChangeStreamCheckInvalidate' type",
            changeStreamCheckInvalidateDS);

    return make_intrusive<exec::agg::ChangeStreamCheckInvalidateStage>(
        changeStreamCheckInvalidateDS->kStageName,
        changeStreamCheckInvalidateDS->getExpCtx(),
        changeStreamCheckInvalidateDS->_startAfterInvalidate);
}

namespace exec::agg {

using DSCS = DocumentSourceChangeStream;

REGISTER_AGG_STAGE_MAPPING(_internalChangeStreamCheckInvalidate,
                           DocumentSourceChangeStreamCheckInvalidate::id,
                           documentSourceChangeStreamCheckInvalidateToStageFn)

namespace {

// Returns true if the given 'operationType' should invalidate the change stream based on the
// namespace in 'pExpCtx'.
bool isInvalidatingCommand(const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                           StringData operationType) {
    if (pExpCtx->isSingleNamespaceAggregation()) {
        return operationType == DSCS::kDropCollectionOpType ||
            operationType == DSCS::kRenameCollectionOpType ||
            operationType == DSCS::kDropDatabaseOpType;
    } else if (!pExpCtx->isClusterAggregation()) {
        return operationType == DSCS::kDropDatabaseOpType;
    } else {
        return false;
    }
};

}  // namespace

ChangeStreamCheckInvalidateStage::ChangeStreamCheckInvalidateStage(
    StringData stageName,
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
    boost::optional<ResumeTokenData> startAfterInvalidate)
    : Stage(stageName, pExpCtx), _startAfterInvalidate(startAfterInvalidate) {}

GetNextResult ChangeStreamCheckInvalidateStage::doGetNext() {
    // To declare a change stream as invalidated, this stage first emits an invalidate event and
    // then throws a 'ChangeStreamInvalidated' exception on the next call to this method.

    if (_queuedInvalidate) {
        auto res = DocumentSource::GetNextResult(std::move(_queuedInvalidate.value()));
        _queuedInvalidate.reset();
        return res;
    }

    if (_queuedException) {
        uasserted(std::move(*_queuedException), "Change stream invalidated");
    }

    auto nextInput = pSource->getNext();
    if (!nextInput.isAdvanced())
        return nextInput;

    auto& doc = nextInput.getDocument();
    DSCS::checkValueType(
        doc[DSCS::kOperationTypeField], DSCS::kOperationTypeField, BSONType::string);
    auto operationType = doc[DSCS::kOperationTypeField].getString();

    // If this command should invalidate the stream, generate an invalidate entry and queue it up
    // to be returned after the notification of this command. The new entry will have a nearly
    // identical resume token to the notification for the command, except with an extra flag
    // indicating that the token is from an invalidate. This flag is necessary to disambiguate
    // the two tokens, and thus preserve a total ordering on the stream.
    if (isInvalidatingCommand(pExpCtx, operationType)) {
        // Regardless of whether we generate an invalidation event or, in the case of startAfter,
        // swallow it, we should clear the _startAfterInvalidate field once this block completes.
        ON_BLOCK_EXIT([this] { _startAfterInvalidate.reset(); });

        // Extract the resume token from the invalidating command and set the 'fromInvalidate' bit.
        auto resumeTokenData = ResumeToken::parse(doc[DSCS::kIdField].getDocument()).getData();
        resumeTokenData.fromInvalidate = ResumeTokenData::FromInvalidate::kFromInvalidate;

        // If a client receives an invalidate and wants to start a new stream after the invalidate,
        // they can use the 'startAfter' option. In this case, '_startAfterInvalidate' will be set
        // to the resume token with which the client restarted the stream. We must be sure to avoid
        // re-invalidating the new stream, and so we will swallow the first invalidate we see on
        // each shard. The one exception is the invalidate which matches the 'startAfter' resume
        // token. We must re-generate this invalidate, since DSEnsureResumeTokenPresent needs to see
        // (and will take care of swallowing) the event which exactly matches the client's token.
        if (_startAfterInvalidate && resumeTokenData != *_startAfterInvalidate) {
            return nextInput;
        }

        auto resumeTokenDoc = ResumeToken(resumeTokenData).toDocument();

        // Note: if 'showExpandedEvents' is false, 'wallTime' will be missing in the input document.
        MutableDocument result(Document{{DSCS::kIdField, resumeTokenDoc},
                                        {DSCS::kOperationTypeField, DSCS::kInvalidateOpType},
                                        {DSCS::kClusterTimeField, doc[DSCS::kClusterTimeField]},
                                        {DSCS::kWallTimeField, doc[DSCS::kWallTimeField]}});
        result.copyMetaDataFrom(doc);

        // We set the resume token as the document's sort key in both the sharded and non-sharded
        // cases, since we will later rely upon it to generate a correct postBatchResumeToken. We
        // must therefore update the sort key to match the new resume token that we generated above.
        const bool isSingleElementKey = true;
        result.metadata().setSortKey(Value{resumeTokenDoc}, isSingleElementKey);

        // If we are here and '_startAfterInvalidate' is present, then the current event matches the
        // resume token. We throw the event up to DSCSEnsureResumeTokenPresent, to ensure that it is
        // always delivered regardless of any intervening $match stages.
        uassert(ChangeStreamStartAfterInvalidateInfo(result.freeze().toBsonWithMetaData()),
                "Change stream 'startAfter' invalidate event",
                !_startAfterInvalidate);

        // Otherwise, we are in a normal invalidation scenario. Queue up an invalidation event to be
        // returned on the following call to getNext, and an invalidation exception to be thrown on
        // the call after that.
        _queuedInvalidate = result.freeze();
        _queuedException = ChangeStreamInvalidationInfo(
            _queuedInvalidate->metadata().getSortKey().getDocument().toBson());
    }

    // Regardless of whether the first document we see is an invalidating command, we only skip the
    // first invalidate for streams with the 'startAfter' option, so we should not skip any
    // invalidates that come after the first one.
    _startAfterInvalidate.reset();

    return nextInput;
}

}  // namespace exec::agg
}  // namespace mongo
