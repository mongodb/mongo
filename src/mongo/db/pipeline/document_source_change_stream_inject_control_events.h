// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/document_source_change_stream_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/util/modules.h"
#include "mongo/util/string_map.h"

#include <string_view>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

DECLARE_STAGE_PARAMS_DERIVED_DEFAULT(ChangeStreamInjectControlEvents);
using ChangeStreamInjectControlEventsLiteParsed =
    DocumentSourceChangeStreamLiteParsedInternal<ChangeStreamInjectControlEventsStageParams>;

/**
 * This pipeline stage can turn specific events into so-called 'control events'. It can either be
 * configured to turn certain event types into control events, by setting the control events flag on
 * them. Alternatively, it can pass on such events unmodified, but inject a control event after the
 * original event.
 */
class DocumentSourceChangeStreamInjectControlEvents final
    : public DocumentSourceInternalChangeStreamStage {
public:
    static constexpr std::string_view kStageName = "$_internalChangeStreamInjectControlEvents"sv;

    // Constants for action types that this stage can perform.
    static constexpr std::string_view kActionNameInjectControlEvent = "injectControlEvent"sv;
    static constexpr std::string_view kActionNameTransformToControlEvent =
        "transformToControlEvent"sv;

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

        /**
         * Build the action map for a data shard from the current change stream specification.
         */
        static ActionsMap buildMapForDataShard(
            const boost::intrusive_ptr<ExpressionContext>& expCtx,
            const DocumentSourceChangeStreamSpec& spec);
    };

    std::string_view getSourceName() const override;

    StageConstraints constraints(PipelineSplitState pipeState) const override {
        StageConstraints constraints(StreamType::kStreaming,
                                     PositionRequirement::kNone,
                                     HostTypeRequirement::kTargetedShards,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kNotAllowed,
                                     TransactionRequirement::kNotAllowed,
                                     LookupRequirement::kNotAllowed,
                                     UnionRequirement::kNotAllowed,
                                     ChangeStreamRequirement::kChangeStreamStage);
        constraints.consumesLogicalCollectionData = false;
        constraints.outputDependsOnSingleInput = true;
        return constraints;
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic(
        const DistributedPlanContext* ctx) final {
        return boost::none;
    }

    DepsTracker::State getDependencies(DepsTracker* deps) const final {
        deps->fields.insert(std::string{DocumentSourceChangeStream::kOperationTypeField});
        return DepsTracker::State::SEE_NEXT;
    }

    Value doSerialize(const query_shape::SerializationOptions& opts =
                          query_shape::SerializationOptions{}) const override;

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

    static boost::intrusive_ptr<DocumentSourceChangeStreamInjectControlEvents> createFromBson(
        BSONElement spec, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    static boost::intrusive_ptr<DocumentSourceChangeStreamInjectControlEvents> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx, const BSONObj& actions = {});

    static boost::intrusive_ptr<DocumentSourceChangeStreamInjectControlEvents> createForDataShard(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const DocumentSourceChangeStreamSpec& spec);

    static const Id& id;

    Id getId() const override {
        return id;
    }

    const ActionsMap& getActionsMap_forTest() const {
        return _actions;
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
