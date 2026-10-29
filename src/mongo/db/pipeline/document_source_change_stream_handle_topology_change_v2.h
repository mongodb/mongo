// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/change_stream_constants.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/modules.h"

#include <set>
#include <string_view>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

/**
 * An internal DocumentSource used as part of the V2 change streams infrastructure, to listen for
 * events that describe topology changes to the cluster. This stage is responsible for opening and
 * closing remote cursors on these shards as needed.
 */
class DocumentSourceChangeStreamHandleTopologyChangeV2 final
    : public DocumentSourceInternalChangeStreamStage {
public:
    static constexpr std::string_view kStageName =
        change_stream_constants::stage_names::kHandleTopologyChangeV2;

    static boost::intrusive_ptr<DocumentSourceChangeStreamHandleTopologyChangeV2> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    /**
     * Creates a new stage for V2 change stream readers.
     * The stage parametrization will happen according to the change stream-related configuration in
     * the provided 'ExpressionContext' and server parameters.
     */
    static boost::intrusive_ptr<DocumentSourceChangeStreamHandleTopologyChangeV2> create(
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
        return DistributedPlanLogic{nullptr, this, change_stream_constants::kSortSpec};
    }

    DepsTracker::State getDependencies(DepsTracker* deps) const final {
        deps->setNeedsMetadata(DocumentMetadataFields::MetaType::kSortKey);
        return DepsTracker::State::SEE_NEXT;
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

    static const Id& id;

    Id getId() const override {
        return id;
    }

private:
    DocumentSourceChangeStreamHandleTopologyChangeV2(
        const boost::intrusive_ptr<ExpressionContext>& expCtx);
};

}  // namespace mongo
