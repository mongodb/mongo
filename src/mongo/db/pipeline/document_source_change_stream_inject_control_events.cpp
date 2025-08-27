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

#include "mongo/db/pipeline/document_source_change_stream_inject_control_events.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <set>

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

using boost::intrusive_ptr;

namespace mongo {

REGISTER_INTERNAL_DOCUMENT_SOURCE(_internalChangeStreamInjectControlEvents,
                                  LiteParsedDocumentSourceChangeStreamInternal::parse,
                                  DocumentSourceChangeStreamInjectControlEvents::createFromBson,
                                  true);

ALLOCATE_DOCUMENT_SOURCE_ID(_internalChangeStreamInjectControlEvents,
                            DocumentSourceChangeStreamInjectControlEvents::id)

namespace {

constexpr StringData kActionsName = "actions"_sd;

}  // namespace

DocumentSourceChangeStreamInjectControlEvents::ActionsMap
DocumentSourceChangeStreamInjectControlEvents::ActionsHelper::parseFromBSON(
    const BSONObj& actions) {
    ActionsMap result;

    auto getActionFromName = [](StringData actionName) {
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
    std::set<StringData> keys;
    for (const auto& action : actions) {
        keys.insert(action.first);
    }

    // Iterate over sorted keys so that we have deterministically ordered output.
    BSONObjBuilder bob;
    for (auto key : keys) {
        auto it = actions.find(key);
        tassert(10384003, "key must exist in actions map", it != actions.end());
        const auto& action = *it;
        StringData actionName = action.second == Action::kTransformToControlEvent
            ? kActionNameTransformToControlEvent
            : kActionNameInjectControlEvent;
        bob.append(action.first /* event name */, actionName);
    }
    return bob.obj();
}

DocumentSourceChangeStreamInjectControlEvents::DocumentSourceChangeStreamInjectControlEvents(
    const intrusive_ptr<ExpressionContext>& expCtx,
    DocumentSourceChangeStreamInjectControlEvents::ActionsMap actions)
    : DocumentSourceInternalChangeStreamStage(getSourceName(), expCtx),
      _actions(std::move(actions)) {}

intrusive_ptr<DocumentSourceChangeStreamInjectControlEvents>
DocumentSourceChangeStreamInjectControlEvents::create(
    const intrusive_ptr<ExpressionContext>& expCtx,
    const DocumentSourceChangeStreamSpec& spec,
    const BSONObj& actions) {
    return new DocumentSourceChangeStreamInjectControlEvents(expCtx,
                                                             ActionsHelper::parseFromBSON(actions));
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

const char* DocumentSourceChangeStreamInjectControlEvents::getSourceName() const {
    return kStageName.data();
}

Value DocumentSourceChangeStreamInjectControlEvents::doSerialize(
    const SerializationOptions& opts) const {
    if (opts.isReplacingLiteralsWithRepresentativeValues()) {
        // 'actions' is an internal parameter. Don't serialize it for representative query shapes.
        return Value();
    }
    BSONObjBuilder builder;
    if (opts.isSerializingForExplain()) {
        BSONObjBuilder sub(builder.subobjStart(DocumentSourceChangeStream::kStageName));
        sub.append("stage"_sd, kStageName);
        sub << kActionsName << ActionsHelper::serializeToBSON(_actions);
        sub.done();
    } else {
        builder.append(kStageName, BSON(kActionsName << ActionsHelper::serializeToBSON(_actions)));
    }
    return Value(builder.obj());
}

}  // namespace mongo
