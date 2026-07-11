// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/change_stream_check_invalidate_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/pipeline/change_stream_start_after_invalidate_info.h"
#include "mongo/db/pipeline/document_source_change_stream_check_invalidate.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

#include <string_view>

namespace mongo {

boost::intrusive_ptr<exec::agg::Stage> documentSourceChangeStreamCheckInvalidateToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    auto* changeStreamCheckInvalidateDS =
        dynamic_cast<DocumentSourceChangeStreamCheckInvalidate*>(documentSource.get());

    tassert(10561302,
            "expected 'DocumentSourceChangeStreamCheckInvalidate' type",
            changeStreamCheckInvalidateDS);

    const auto& expCtx = changeStreamCheckInvalidateDS->getExpCtx();

    // The following assert verifies that the '$_internalChangeStreamCheckInvalidate' stage is
    // only present in change stream pipelines that actually need it, i.e. collection-level and
    // database-level change streams. Versions before v8.3 still create the invalidate stage for
    // all types of change streams unconditionally. A mongos from an old version can still send
    // a pipeline containing an invalidate stage even though it is not needed since v8.3 and
    // higher. To make multiversion setups and upgrades work, we restrict the check to the
    // router.
    tassert(11073200,
            str::stream()
                << "expecting 'DocumentSourceChangeStreamCheckInvalidate' to be built only for "
                   "collection-level or database-level change stream pipeline on the router, "
                   "got nss: '"
                << expCtx->getNamespaceString().toStringForErrorMsg() << "'",
            !expCtx->getInRouter() ||
                DocumentSourceChangeStreamCheckInvalidate::canInvalidateEventOccur(expCtx));

    return make_intrusive<exec::agg::ChangeStreamCheckInvalidateStage>(
        changeStreamCheckInvalidateDS->kStageName,
        expCtx,
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
bool isInvalidationCommand(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                           std::string_view operationType) {
    if (expCtx->isSingleNamespaceAggregation()) {
        return operationType == DSCS::kDropCollectionOpType ||
            operationType == DSCS::kRenameCollectionOpType ||
            operationType == DSCS::kDropDatabaseOpType;
    } else if (!expCtx->isClusterAggregation()) {
        return operationType == DSCS::kDropDatabaseOpType;
    }
    return false;
}

}  // namespace

ChangeStreamCheckInvalidateStage::ChangeStreamCheckInvalidateStage(
    std::string_view stageName,
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
    boost::optional<ResumeTokenData> startAfterInvalidate)
    : Stage(stageName, pExpCtx), _startAfterInvalidate(std::move(startAfterInvalidate)) {}

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
    if (!nextInput.isAdvanced()) {
        return nextInput;
    }

    const auto& doc = nextInput.getDocument();

    ON_BLOCK_EXIT([this] { _startAfterInvalidate.reset(); });

    // If it's not an invalidation event, just forward the event.
    const auto& operationType = doc[DSCS::kOperationTypeField];
    DSCS::checkValueType(operationType, DSCS::kOperationTypeField, BSONType::string);

    if (!isInvalidationCommand(pExpCtx, operationType.getString())) {
        return nextInput;
    }

    // Extract the resume token from the invalidating command and set the 'fromInvalidate' bit.
    auto resumeTokenData = ResumeToken::parse(doc.metadata().getSortKey().getDocument()).getData();
    resumeTokenData.fromInvalidate = ResumeTokenData::FromInvalidate::kFromInvalidate;

    switch (_classifyInvalidationForStartAfter(resumeTokenData)) {
        case ClassificationType::kRethrowForResumeTokenVerification: {
            Document result = _buildInvalidateEvent(resumeTokenData, doc);
            uasserted(ChangeStreamStartAfterInvalidateInfo(result.toBsonWithMetaData()),
                      "Change stream 'startAfter' invalidate event");
        }
        case ClassificationType::kGenerateInvalidateEvent: {
            _queuedInvalidate = _buildInvalidateEvent(resumeTokenData, doc);
            _queuedException = ChangeStreamInvalidationInfo(
                _queuedInvalidate->metadata().getSortKey().getDocument().toBson());
        }
            [[fallthrough]];
        case ClassificationType::kSwallow: {
            return nextInput;
        }
    }

    MONGO_UNREACHABLE_TASSERT(12064400);
}

ChangeStreamCheckInvalidateStage::ClassificationType
ChangeStreamCheckInvalidateStage::_classifyInvalidationForStartAfter(
    const ResumeTokenData& resumeTokenData) const {
    if (!_startAfterInvalidate) {
        return ClassificationType::kGenerateInvalidateEvent;
    }

    if (resumeTokenData == *_startAfterInvalidate) {
        // Exact match — this is the event the client resumed from. Rethrow it so
        // EnsureResumeTokenPresent can verify the resume point.
        return ClassificationType::kRethrowForResumeTokenVerification;
    }

    if (resumeTokenData.clusterTime > _startAfterInvalidate->clusterTime) {
        // This event has a strictly later clusterTime than the resume point. It is a
        // genuinely new invalidation — not part of the original event we're resuming from.
        return ClassificationType::kGenerateInvalidateEvent;
    }

    // Different token at the same or earlier clusterTime. Since this
    // is part of the same logical event the client already moved past, suppress the invalidation.
    return ClassificationType::kSwallow;
}

Document ChangeStreamCheckInvalidateStage::_buildInvalidateEvent(
    const ResumeTokenData& resumeTokenData, const Document& doc) const {
    auto resumeTokenDoc = ResumeToken(resumeTokenData).toDocument();

    // Note: if 'showExpandedEvents' is false, 'wallTime' will be missing in the input
    // document.
    MutableDocument result(Document{{DSCS::kIdField, resumeTokenDoc},
                                    {DSCS::kOperationTypeField, DSCS::kInvalidateOpType},
                                    {DSCS::kClusterTimeField, doc[DSCS::kClusterTimeField]},
                                    {DSCS::kWallTimeField, doc[DSCS::kWallTimeField]}});
    result.copyMetaDataFrom(doc);

    // We set the resume token as the document's sort key in both the sharded and
    // non-sharded cases, since we will later rely upon it to generate a correct
    // postBatchResumeToken. We must therefore update the sort key to match the new resume
    // token that we generated above.
    const bool isSingleElementKey = true;
    result.metadata().setSortKey(Value{resumeTokenDoc}, isSingleElementKey);
    return result.freeze();
}

}  // namespace exec::agg
}  // namespace mongo
