// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/change_stream_check_invalidate_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/pipeline/change_stream_hashed_field_accessors.h"
#include "mongo/db/pipeline/change_stream_start_after_invalidate_info.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/document_source_change_stream_check_invalidate.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

#include <compare>
#include <string_view>

namespace mongo {
using FieldAccessors = change_stream::HashedFieldAccessors;

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
        changeStreamCheckInvalidateDS->_startAfterInvalidate,
        changeStreamCheckInvalidateDS->_resumePoint);
}

namespace exec::agg {

REGISTER_AGG_STAGE_MAPPING(_internalChangeStreamCheckInvalidate,
                           DocumentSourceChangeStreamCheckInvalidate::id,
                           documentSourceChangeStreamCheckInvalidateToStageFn)

namespace {

// Returns true if the given 'operationType' should invalidate the change stream based on the
// namespace in 'pExpCtx'.
bool isInvalidationCommand(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                           std::string_view operationType) {
    if (expCtx->isSingleNamespaceAggregation()) {
        return operationType == DocumentSourceChangeStream::kDropCollectionOpType ||
            operationType == DocumentSourceChangeStream::kRenameCollectionOpType ||
            operationType == DocumentSourceChangeStream::kDropDatabaseOpType;
    } else if (!expCtx->isClusterAggregation()) {
        return operationType == DocumentSourceChangeStream::kDropDatabaseOpType;
    }
    return false;
}

}  // namespace

ChangeStreamCheckInvalidateStage::ChangeStreamCheckInvalidateStage(
    std::string_view stageName,
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
    boost::optional<ResumeTokenData> startAfterInvalidate,
    boost::optional<ResumeTokenData> resumePoint)
    : Stage(stageName, pExpCtx),
      _startAfterInvalidate(std::move(startAfterInvalidate)),
      _resumePoint(std::move(resumePoint)) {}

GetNextResult ChangeStreamCheckInvalidateStage::doGetNext() {
    // To declare a change stream as invalidated, this stage first emits an invalidate event and
    // then throws a 'ChangeStreamInvalidated' exception on the next call to this method.
    if (MONGO_unlikely(_queuedInvalidate)) {
        auto res = DocumentSource::GetNextResult(std::move(_queuedInvalidate.value()));
        _queuedInvalidate.reset();
        return res;
    }

    if (MONGO_unlikely(_queuedException)) {
        uasserted(std::move(*_queuedException), "Change stream invalidated");
    }

    auto nextInput = pSource->getNext();
    if (!nextInput.isAdvanced()) {
        return nextInput;
    }

    const auto& doc = nextInput.getDocument();

    // Without a resume point (a stream that was not rebuilt) the 'startAfter' state only matters
    // for the first document, as it always has. A stream rebuilt for an end-of-transaction lookback
    // scans the oplog from before its resume point, so the first documents it observes can be
    // pre-resume history; clearing the state on such a document would make the resume-point
    // invalidate look like a new invalidation and wrongly close the stream. The state is therefore
    // kept until the stream reaches the resume point, and '_classifyInvalidation' clears it once it
    // does, so that a command after the resume point is still treated as a genuinely new
    // invalidation.
    ON_BLOCK_EXIT([this] {
        if (!_resumePoint) {
            _startAfterInvalidate.reset();
        }
    });

    // If it's not an invalidation event, just forward the event.
    const auto& operationType = doc[FieldAccessors::kOperationType];
    DocumentSourceChangeStream::checkValueType(
        operationType, DocumentSourceChangeStream::kOperationTypeField, BSONType::string);

    if (!isInvalidationCommand(pExpCtx, operationType.getString())) {
        return nextInput;
    }

    // Extract the resume token from the invalidating command and set the 'fromInvalidate' bit.
    auto resumeTokenData = ResumeToken::parse(doc.metadata().getSortKey().getDocument()).getData();
    resumeTokenData.fromInvalidate = ResumeTokenData::FromInvalidate::kFromInvalidate;

    switch (_classifyInvalidation(resumeTokenData)) {
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
ChangeStreamCheckInvalidateStage::_classifyInvalidation(const ResumeTokenData& resumeTokenData) {
    // A stream rebuilt for an end-of-transaction lookback scans the oplog from before its resume
    // point, so it can observe an invalidating command the stream has already moved past. Such an
    // event is not an invalidation: the stream must not be closed by history it has already
    // processed. This compares the full resume token, not only its cluster time, because a command
    // at the same cluster time can still sort before the resume point (for example, an event
    // ordered against a cross-shard high-water mark by its shard UUID).
    bool reachedResumePoint = false;
    if (_resumePoint) {
        const std::partial_ordering ordering = resumeTokenData <=> *_resumePoint;
        if (ordering == std::partial_ordering::less ||
            ordering == std::partial_ordering::unordered) {
            // Strictly before the resume point, or incomparable because the event identities use
            // different versions: history the stream has already passed.
            return ClassificationType::kSwallow;
        }
        reachedResumePoint = true;
    }

    // At or after the resume point: fall through to the shared 'startAfter' classification below so
    // that a rebuilt stream behaves exactly like a stream that was not rebuilt. The resume point
    // and the 'startAfter' token are the same client token in the rebuild path, so the ordering
    // alone cannot tell the resume-point event apart from a different command at its cluster time.
    // Once the stream has reached the resume point, drop the 'startAfter' state after the current
    // event has been classified, so that a later command at the resume point's cluster time is a
    // genuinely new invalidation; retaining it would suppress such a command for the whole rebuilt
    // stream, unlike a stream that was not rebuilt.
    ON_BLOCK_EXIT([this, reachedResumePoint] {
        if (reachedResumePoint) {
            _startAfterInvalidate.reset();
        }
    });

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
    MutableDocument result(Document{{DocumentSourceChangeStream::kIdField, resumeTokenDoc},
                                    {DocumentSourceChangeStream::kOperationTypeField,
                                     DocumentSourceChangeStream::kInvalidateOpType},
                                    {DocumentSourceChangeStream::kClusterTimeField,
                                     doc[DocumentSourceChangeStream::kClusterTimeField]},
                                    {DocumentSourceChangeStream::kWallTimeField,
                                     doc[DocumentSourceChangeStream::kWallTimeField]}});
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
