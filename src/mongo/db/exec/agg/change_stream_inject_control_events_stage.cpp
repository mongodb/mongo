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

#include "mongo/db/exec/agg/change_stream_inject_control_events_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/util/assert_util.h"

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
    StringData stageName,
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
