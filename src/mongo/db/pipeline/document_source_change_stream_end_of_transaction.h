// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/pipeline/change_stream_constants.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/modules.h"

#include <memory>
#include <set>
#include <string_view>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

class Pipeline;

/**
 * An internal router-side DocumentSource used as part of the V2 change streams infrastructure to
 * synthesize user-visible 'endOfTransaction' events from the per-shard accounting control events
 * delivered through the topology split point.
 *
 * It sits immediately after 'DocumentSourceChangeStreamHandleTopologyChangeV2' and is only ever
 * added to the router part of a V2 change stream pipeline.
 */
class DocumentSourceChangeStreamEndOfTransaction final
    : public DocumentSourceInternalChangeStreamStage {
public:
    static constexpr std::string_view kStageName =
        change_stream_constants::stage_names::kEndOfTransaction;

    static boost::intrusive_ptr<DocumentSourceChangeStreamEndOfTransaction> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    /**
     * Creates a new synthesis stage for V2 change stream readers opened on a router.
     */
    static boost::intrusive_ptr<DocumentSourceChangeStreamEndOfTransaction> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx);

    std::string_view getSourceName() const final {
        return kStageName;
    }

    Value doSerialize(const query_shape::SerializationOptions& opts =
                          query_shape::SerializationOptions{}) const final;

    StageConstraints constraints(PipelineSplitState pipeState) const final;

    GetModPathsReturn getModifiedPaths() const final {
        // This stage neither modifies nor renames any field.
        return {GetModPathsReturn::Type::kFiniteSet, OrderedPathSet{}, {}};
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic(
        const DistributedPlanContext* ctx) final {
        return boost::none;
    }

    DepsTracker::State getDependencies(DepsTracker* deps) const final {
        deps->setNeedsMetadata(DocumentMetadataFields::MetaType::kSortKey);
        return DepsTracker::State::SEE_NEXT;
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

    /**
     * Sets the user-defined single-document transformation stages that run on the shards, so that a
     * copy of them can be re-applied to the events synthesized by this stage. Called after the
     * router pipeline has been split.
     */
    void setUserPipeline(DocumentSourceContainer userStages);

    /**
     * Returns the user pipeline that is re-applied to synthesized events, or nullptr if there is
     * none.
     */
    const std::shared_ptr<Pipeline>& getUserPipeline() const {
        return _userPipeline;
    }

    static const Id& id;

    Id getId() const override {
        return id;
    }

private:
    friend boost::intrusive_ptr<exec::agg::Stage>
    documentSourceChangeStreamEndOfTransactionToStageFn(
        const boost::intrusive_ptr<DocumentSource>& documentSource);

    DocumentSourceChangeStreamEndOfTransaction(
        const boost::intrusive_ptr<ExpressionContext>& expCtx);

    // A copy of the user-defined single-document transformation stages that run on the shards, in
    // pipeline order. Null when the change stream has no such stages.
    std::shared_ptr<Pipeline> _userPipeline;
};

}  // namespace mongo
