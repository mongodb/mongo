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

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/document_source_change_stream_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/util/string_map.h"

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
/**
 * This pipeline stage can turn specific events into so-called 'control events'. It can either be
 * configured to turn certain event types into control events, by setting the control events flag on
 * them. Alternatively, it can pass on such events unmodified, but inject a control event after the
 * original event.
 */
class DocumentSourceChangeStreamInjectControlEvents final
    : public DocumentSourceInternalChangeStreamStage {
public:
    static constexpr StringData kStageName = "$_internalChangeStreamInjectControlEvents"_sd;

    // Constants for action types that this stage can perform.
    static constexpr StringData kActionNameInjectControlEvent = "injectControlEvent"_sd;
    static constexpr StringData kActionNameTransformToControlEvent = "transformToControlEvent"_sd;

    enum class Action {
        // Consumes the matching change event, transforms the event into a control one, and emits
        // it.
        kTransformToControlEvent,

        // Inserts a control event after the matching change event. The control event is a clone of
        // the matching event transformed into a control event.
        kInjectControlEvent,
    };

    /**
     * Mapping from stage names to actions (injectControlEvent / transformToControlEvent).
     */
    using ActionsMap = StringMap<Action>;

    /**
     * Helper struct for serialization / deserialization.
     */
    struct ActionsHelper {
        /**
         * Convert a BSONObj with an actions description into the internal C++ type.
         */
        static ActionsMap parseFromBSON(const BSONObj& actions);

        /**
         * Convert the internal C++ actions type into a BSONObj.
         */
        static BSONObj serializeToBSON(const ActionsMap& actions);
    };

    const char* getSourceName() const override;

    StageConstraints constraints(PipelineSplitState pipeState) const override {
        StageConstraints constraints(StreamType::kStreaming,
                                     PositionRequirement::kNone,
                                     HostTypeRequirement::kAnyShard,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kNotAllowed,
                                     TransactionRequirement::kNotAllowed,
                                     LookupRequirement::kNotAllowed,
                                     UnionRequirement::kNotAllowed,
                                     ChangeStreamRequirement::kChangeStreamStage);
        constraints.consumesLogicalCollectionData = false;
        return constraints;
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final {
        return boost::none;
    }

    Value doSerialize(const SerializationOptions& opts = SerializationOptions{}) const override;

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

    static boost::intrusive_ptr<DocumentSourceChangeStreamInjectControlEvents> createFromBson(
        BSONElement spec, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    static boost::intrusive_ptr<DocumentSourceChangeStreamInjectControlEvents> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const DocumentSourceChangeStreamSpec& spec,
        const BSONObj& actions = {});

    static const Id& id;

    Id getId() const override {
        return id;
    }

private:
    friend boost::intrusive_ptr<exec::agg::Stage>
    documentSourceChangeStreamInjectControlEventsToStageFn(
        const boost::intrusive_ptr<DocumentSource>& documentSource);

    /**
     * Use the create static method to create a DocumentSourceChangeStreamInjectControlEvents.
     */
    DocumentSourceChangeStreamInjectControlEvents(
        const boost::intrusive_ptr<ExpressionContext>& expCtx, ActionsMap actions);

    /**
     * A mapping from event name to the actions to take.
     */
    ActionsMap _actions;
};
}  // namespace mongo
