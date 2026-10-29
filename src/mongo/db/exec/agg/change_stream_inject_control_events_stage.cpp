// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/change_stream_inject_control_events_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/util/assert_util.h"

#include <string_view>

namespace mongo {

boost::intrusive_ptr<exec::agg::Stage> documentSourceChangeStreamInjectControlEventsToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    auto* changeStreamInjectControlEventsDS =
        dynamic_cast<DocumentSourceChangeStreamInjectControlEvents*>(documentSource.get());

    tassert(10561307,
            "expected 'DocumentSourceChangeStreamInjectControlEvents' type",
            changeStreamInjectControlEventsDS);

    return make_intrusive<exec::agg::ChangeStreamInjectControlEventsStage>(
        changeStreamInjectControlEventsDS->kStageName,
        changeStreamInjectControlEventsDS->getExpCtx(),
        changeStreamInjectControlEventsDS->_actions);
}

namespace exec::agg {

REGISTER_AGG_STAGE_MAPPING(_internalChangeStreamInjectControlEvents,
                           DocumentSourceChangeStreamInjectControlEvents::id,
                           documentSourceChangeStreamInjectControlEventsToStageFn)

ChangeStreamInjectControlEventsStage::ChangeStreamInjectControlEventsStage(
    std::string_view stageName,
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
    DocumentSourceChangeStreamInjectControlEvents::ActionsMap actions)
    : Stage(stageName, pExpCtx), _actions(std::move(actions)) {}

GetNextResult ChangeStreamInjectControlEventsStage::doGetNext() {
    // If there is a buffered control event, return it.
    if (_bufferedControlEvent.has_value()) {
        auto next = std::move(*_bufferedControlEvent);
        tassert(10384004,
                "Expecting buffered event to be a control event",
                next.isAdvancedControlDocument());
        _bufferedControlEvent = boost::none;
        return next;
    }

    GetNextResult next = pSource->getNext();
    if (!next.isAdvanced()) {
        // This stage lets all inputs through as is that are not regular documents.
        return next;
    }

    const Document& nextDoc = next.getDocument();

    // Read 'operationType' field of the event.
    auto opType = nextDoc[DocumentSourceChangeStream::kOperationTypeField];

    // The value of 'operationType' must be a string. We cannot handle anything else.
    if (opType.getType() == BSONType::string) {
        // Check if we have an action registered for the specific event type.
        if (auto itAction = _actions.find(opType.getStringData()); itAction != _actions.end()) {
            // We have an action registered for the specific event type!
            MutableDocument docBuilder(nextDoc);
            docBuilder.metadata().setChangeStreamControlEvent();
            auto controlEvent =
                DocumentSource::GetNextResult::makeAdvancedControlDocument(docBuilder.freeze());

            if (itAction->second ==
                DocumentSourceChangeStreamInjectControlEvents::Action::kTransformToControlEvent) {
                // Transform the event into a control event.
                return controlEvent;
            }

            // Buffer the control event for the next call, and fall through to returning the current
            // document.
            _bufferedControlEvent = std::move(controlEvent);
        }
    }

    // Whatever is emitted here must be a normal, non-control result.
    tassert(10384005, "Expecting event to be a non-control event", next.isAdvanced());
    return next;
}

}  // namespace exec::agg
}  // namespace mongo
