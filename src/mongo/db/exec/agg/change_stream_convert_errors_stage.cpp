// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/change_stream_convert_errors_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/pipeline/change_stream_helpers.h"
#include "mongo/db/pipeline/document_source_change_stream_convert_errors.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"

#include <string_view>
#include <utility>

namespace mongo {

boost::intrusive_ptr<exec::agg::Stage> documentSourceChangeStreamConvertErrorsToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    auto* convertErrorsDS =
        dynamic_cast<DocumentSourceChangeStreamConvertErrors*>(documentSource.get());

    tassert(13731901, "expected 'DocumentSourceChangeStreamConvertErrors' type", convertErrorsDS);

    return make_intrusive<exec::agg::ChangeStreamConvertErrorsStage>(convertErrorsDS->kStageName,
                                                                     convertErrorsDS->getExpCtx());
}

namespace exec::agg {

REGISTER_AGG_STAGE_MAPPING(_internalChangeStreamConvertErrors,
                           DocumentSourceChangeStreamConvertErrors::id,
                           documentSourceChangeStreamConvertErrorsToStageFn)

GetNextResult ChangeStreamConvertErrorsStage::doGetNext() {
    // Drop the recorded identity and error source on every exit path -- whether the event below was
    // delivered, the cursor paused or hit EOF, a bypass/gap event was released, an error event was
    // converted, or an exception escaped this stage (e.g. a rethrow below, or a BSONObjectTooLarge
    // raised while serializing the error event) -- so a failure in a later getNext() is never
    // mislabeled by a stale identity from an earlier event. On the success path this runs after
    // serialization, so the in-flight identity is still available for the oversized-event path to
    // rebuild the error event from the pre-user-stage snapshot; everywhere else it runs as soon as
    // the method leaves, which is exactly what is wanted for pause/EOF and exception exits.
    ScopeGuard clearErrorState{[&] {
        pExpCtx->clearActiveChangeStreamEvent();
        pExpCtx->clearActiveChangeStreamErrorSource();
    }};

    // Gather the document to return: a normal change event, or an error event converted from a
    // recoverable failure raised by the stages below.
    boost::optional<Document> event;
    try {
        auto nextResult = pSource->getNext();
        while (nextResult.isPaused()) {
            nextResult = pSource->getNext();
        }

        if (!nextResult.isAdvanced()) {
            return nextResult;
        }
        event = nextResult.releaseDocument();
    } catch (const ExceptionFor<ErrorCodes::ChangeStreamErrorEventBypass>&) {
        // An operationType:"error" event was thrown past the user stages that could not be pushed
        // to the shards; return it directly, preserving its sort-key metadata.
        auto stashed = pExpCtx->releasePendingChangeStreamRouterErrorEvent();
        tassert(13731902,
                "change stream error event bypass without a stashed event",
                stashed.has_value());
        event = std::move(*stashed);
    } catch (const DBException& ex) {
        // A resume gap detected below the user stages: materialize it here, above them. Only the
        // gap signal itself releases it: the gap must not mask an unrelated failure that raced in
        // ahead of it.
        if (ex.code() == ErrorCodes::ChangeStreamHistoryLost) {
            if (auto gap = pExpCtx->releasePendingChangeStreamResumeGap()) {
                event = std::move(*gap);
            }
        }

        // A recoverable failure from below: build the error event here, above the user stages,
        // labelled by the source the producer set.
        if (!event.has_value()) {
            if (auto errorEvent = change_stream::tryBuildErrorEvent(ex, pExpCtx)) {
                event = std::move(*errorEvent);
            } else {
                // Not convertible to an error event: let the failure propagate. The guard above
                // clears the recorded identity/source on the way out.
                throw;
            }
        }
    }

    // Reaching here implies 'event' was produced by one of the branches above: a delivered event,
    // a released bypass or gap event, or a converted error event. Every path that could leave it
    // empty returned or rethrew.
    tassert(13731903, "expected an event after the conversion branches", event.has_value());

    // Serialize through the same helper the plan executor uses, then hand back a Document backed by
    // that BSON so the executor's own serialization is a cached no-op. All BSON-size handling
    // (including the 'changeStreams.largeEventsFailed' metric) lives in that helper. The in-flight
    // identity is preserved until the guard at the top of this method runs (i.e. after
    // serialization), so the oversized-event path can rebuild the error event from the pre-user
    // stage snapshot, then it is cleared so a later failure is never attributed to this (now
    // delivered) event.
    return GetNextResult{change_stream::makeExecutorReadyChangeStreamEvent(
        change_stream::serializeChangeStreamEventForExecutor(std::move(*event), pExpCtx))};
}

}  // namespace exec::agg
}  // namespace mongo
