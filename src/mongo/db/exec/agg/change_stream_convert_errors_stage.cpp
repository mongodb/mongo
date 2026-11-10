// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/change_stream_convert_errors_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/pipeline/change_stream_helpers.h"
#include "mongo/db/pipeline/document_source_change_stream_convert_errors.h"
#include "mongo/util/assert_util.h"

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
    // Gather the document to return: a normal change event, or an error event converted from a
    // recoverable failure raised by the stages below.
    boost::optional<Document> event;
    try {
        auto nextResult = pSource->getNext();
        while (nextResult.isPaused()) {
            nextResult = pSource->getNext();
        }

        // The event in flight has been delivered; drop its identity so a later failure is never
        // attributed to it. Drop the error-source slot too: a rethrow that escaped the conversion
        // point must not mislabel a later failure.
        pExpCtx->clearActiveChangeStreamEvent();
        pExpCtx->clearActiveChangeStreamErrorSource();

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
        // The resumed error event that the bypass carried has been delivered; drop the recorded
        // identity and error source so a later failure is never attributed to it.
        pExpCtx->clearActiveChangeStreamEvent();
        pExpCtx->clearActiveChangeStreamErrorSource();
    } catch (const DBException& ex) {
        // A resume gap detected below the user stages: materialize it here, above them. Only the
        // gap signal itself releases it: the gap must not mask an unrelated failure that raced in
        // ahead of it.
        if (ex.code() == ErrorCodes::ChangeStreamHistoryLost) {
            if (auto gap = pExpCtx->releasePendingChangeStreamResumeGap()) {
                event = std::move(*gap);
                // The gap has been delivered; drop the recorded identity and error source so a
                // later failure is never attributed to it.
                pExpCtx->clearActiveChangeStreamEvent();
                pExpCtx->clearActiveChangeStreamErrorSource();
            }
        }
        // A recoverable failure from below: build the error event here, above the user stages,
        // labelled by the source the producer set. 'tryBuildErrorEvent' is pure, so this stage
        // owns releasing the recorded identity/source slots once they've been consumed.
        if (!event.has_value()) {
            if (auto errorEvent = change_stream::tryBuildErrorEvent(ex, pExpCtx)) {
                event = std::move(*errorEvent);
                // The identity was consumed to build the error event; clear it and the source slot.
                pExpCtx->clearActiveChangeStreamEvent();
                pExpCtx->clearActiveChangeStreamErrorSource();
            } else {
                throw;
            }
        }
    }

    // Reaching here implies 'event' was produced by one of the branches above: a delivered event,
    // a released bypass or gap event, or a converted error event. Every path that could leave it
    // empty returned or rethrew.
    tassert(13731903, "expected an event after the conversion branches", event.has_value());

    return GetNextResult{std::move(*event)};
}

}  // namespace exec::agg
}  // namespace mongo
