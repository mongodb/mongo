// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_change_stream_inject_control_events.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/change_stream_reader_builder.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/s/change_streams/control_events.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <set>
#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

using boost::intrusive_ptr;

namespace mongo {
using namespace std::literals::string_view_literals;

REGISTER_INTERNAL_LITE_PARSED_DOCUMENT_SOURCE(_internalChangeStreamInjectControlEvents,
                                              ChangeStreamInjectControlEventsLiteParsed::parse);

REGISTER_DOCUMENT_SOURCE_WITH_STAGE_PARAMS_DEFAULT(_internalChangeStreamInjectControlEvents,
                                                   DocumentSourceChangeStreamInjectControlEvents,
                                                   ChangeStreamInjectControlEventsStageParams);

ALLOCATE_DOCUMENT_SOURCE_ID(_internalChangeStreamInjectControlEvents,
                            DocumentSourceChangeStreamInjectControlEvents::id)

namespace {

constexpr std::string_view kActionsName = "actions"sv;

}  // namespace

DocumentSourceChangeStreamInjectControlEvents::ActionsMap
DocumentSourceChangeStreamInjectControlEvents::ActionsHelper::parseFromBSON(
    const BSONObj& actions) {
    ActionsMap result;

    auto getActionFromName = [](std::string_view actionName) {
        if (actionName == kActionNameInjectControlEvent) {
            return Action::kInjectControlEvent;
        }
        if (actionName == kActionNameTransformToControlEvent) {
            return Action::kTransformToControlEvent;
        }
        tasserted(10384001,
                  str::stream() << "Invalid action type '" << actionName << "' found in "
                                << kStageName << ": " << actionName);
    };
    for (auto&& [eventName, elem] : actions) {
        Action action = getActionFromName(elem.valueStringDataSafe());
        tassert(10384002,
                str::stream() << "Duplicate action found for event " << eventName << " in "
                              << kStageName,
                result.emplace(eventName, action).second);
    }
    return result;
}

BSONObj DocumentSourceChangeStreamInjectControlEvents::ActionsHelper::serializeToBSON(
    const DocumentSourceChangeStreamInjectControlEvents::ActionsMap& actions) {
    // Copy field names into a get to have a deterministic order of fields for serialization.
    std::set<std::string_view> keys;
    for (const auto& action : actions) {
        keys.insert(action.first);
    }

    // Iterate over sorted keys so that we have deterministically ordered output.
    BSONObjBuilder bob;
    for (auto key : keys) {
        auto it = actions.find(key);
        tassert(10384003, "key must exist in actions map", it != actions.end());
        const auto& action = *it;
        std::string_view actionName = action.second == Action::kTransformToControlEvent
            ? kActionNameTransformToControlEvent
            : kActionNameInjectControlEvent;
        bob.append(action.first /* event name */, actionName);
    }
    return bob.obj();
}

DocumentSourceChangeStreamInjectControlEvents::ActionsMap
DocumentSourceChangeStreamInjectControlEvents::ActionsHelper::buildMapForDataShard(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const DocumentSourceChangeStreamSpec& spec) {
    ChangeStreamReaderBuilder* readerBuilder =
        ChangeStreamReaderBuilder::get(expCtx->getOperationContext()->getServiceContext());

    tassert(10743900, "expecting ChangeStreamReaderBuilder to be available", readerBuilder);

    BSONObjBuilder controlEventsBuilder;
    for (const auto& eventType : readerBuilder->getControlEventTypesOnDataShard(
             expCtx->getOperationContext(), ChangeStream::buildFromExpressionContext(expCtx))) {
        if (eventType == MoveChunkControlEvent::opType && spec.getShowSystemEvents()) {
            controlEventsBuilder.append(eventType, kActionNameInjectControlEvent);
        } else {
            controlEventsBuilder.append(
                eventType,
                DocumentSourceChangeStreamInjectControlEvents::kActionNameTransformToControlEvent);
        }
    }
    return parseFromBSON(controlEventsBuilder.obj());
}

DocumentSourceChangeStreamInjectControlEvents::DocumentSourceChangeStreamInjectControlEvents(
    const intrusive_ptr<ExpressionContext>& expCtx,
    DocumentSourceChangeStreamInjectControlEvents::ActionsMap actions)
    : DocumentSourceInternalChangeStreamStage(getSourceName(), expCtx),
      _actions(std::move(actions)) {}

intrusive_ptr<DocumentSourceChangeStreamInjectControlEvents>
DocumentSourceChangeStreamInjectControlEvents::create(
    const intrusive_ptr<ExpressionContext>& expCtx, const BSONObj& actions) {
    return new DocumentSourceChangeStreamInjectControlEvents(expCtx,
                                                             ActionsHelper::parseFromBSON(actions));
}

intrusive_ptr<DocumentSourceChangeStreamInjectControlEvents>
DocumentSourceChangeStreamInjectControlEvents::createForDataShard(
    const intrusive_ptr<ExpressionContext>& expCtx, const DocumentSourceChangeStreamSpec& spec) {
    return new DocumentSourceChangeStreamInjectControlEvents(
        expCtx, ActionsHelper::buildMapForDataShard(expCtx, spec));
}


intrusive_ptr<DocumentSourceChangeStreamInjectControlEvents>
DocumentSourceChangeStreamInjectControlEvents::createFromBson(
    BSONElement spec, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(10384000,
            str::stream() << "the '" << kStageName << "' object spec must be an object",
            spec.type() == BSONType::object);

    auto parsed = DocumentSourceChangeStreamInjectControlEventsSpec::parse(
        spec.embeddedObject(),
        IDLParserContext("DocumentSourceChangeStreamInjectControlEventsSpec"));

    return new DocumentSourceChangeStreamInjectControlEvents(
        expCtx, ActionsHelper::parseFromBSON(parsed.getActions()));
}

std::string_view DocumentSourceChangeStreamInjectControlEvents::getSourceName() const {
    return kStageName;
}

Value DocumentSourceChangeStreamInjectControlEvents::doSerialize(
    const query_shape::SerializationOptions& opts) const {
    if (opts.isReplacingLiteralsWithRepresentativeValues()) {
        // 'actions' is an internal parameter. Don't serialize it for representative query shapes.
        return Value();
    }
    BSONObjBuilder builder;
    if (opts.isSerializingForExplain()) {
        BSONObjBuilder sub(builder.subobjStart(DocumentSourceChangeStream::kStageName));
        sub.append("stage"sv, kStageName);
        sub << kActionsName << ActionsHelper::serializeToBSON(_actions);
        sub.done();
    } else {
        builder.append(kStageName, BSON(kActionsName << ActionsHelper::serializeToBSON(_actions)));
    }
    return Value(builder.obj());
}

}  // namespace mongo
