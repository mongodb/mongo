// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/modules.h"

#include <set>
#include <string_view>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

DEFINE_LITE_PARSED_STAGE_DEFAULT_DERIVED(InternalSplitPipeline);

/**
 * An internal stage available for testing. Acts as a simple passthrough of intermediate results
 * from the source stage, but forces the pipeline to split at the point where this stage appears
 * (assuming that no earlier splitpoints exist). Takes a single parameter, 'mergeType', which can be
 * one of 'anyShard' or 'router' to control where the merge may occur. Omitting this parameter or
 * specifying 'router' produces the default merging behaviour; the merge half of the  pipeline will
 * be executed on router if all other stages are eligible, and will be sent to a random
 * participating shard otherwise.
 */
class DocumentSourceInternalSplitPipeline final : public DocumentSource {
public:
    static constexpr std::string_view kStageName = "$_internalSplitPipeline"sv;

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement, const boost::intrusive_ptr<ExpressionContext>&);

    static boost::intrusive_ptr<DocumentSource> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        HostTypeRequirement mergeType,
        boost::optional<ShardId> mergeShardId = boost::none) {
        return new DocumentSourceInternalSplitPipeline(expCtx, mergeType, mergeShardId);
    }

    std::string_view getSourceName() const final {
        return kStageName;
    }

    static const Id& id;

    Id getId() const override {
        return id;
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic(
        const DistributedPlanContext* ctx) final {
        // {shardsStage, mergingStage, sortPattern}
        return DistributedPlanLogic{nullptr, this, boost::none};
    }

    StageConstraints constraints(PipelineSplitState pipeState) const final {
        StageConstraints constraints{StreamType::kStreaming,
                                     PositionRequirement::kNone,
                                     _mergeType,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kAllowed,
                                     TransactionRequirement::kAllowed,
                                     _mergeType == HostTypeRequirement::kRouter
                                         ? LookupRequirement::kNotAllowed
                                         : LookupRequirement::kAllowed,
                                     UnionRequirement::kAllowed};
        constraints.preservesCardinality = true;
        constraints.outputDependsOnSingleInput = true;
        if (_mergeShardId) {
            constraints.mergeShardId = _mergeShardId;
        }
        return constraints;
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

    DepsTracker::State getDependencies(DepsTracker* deps) const override {
        return DepsTracker::State::SEE_NEXT;
    }

private:
    DocumentSourceInternalSplitPipeline(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                        HostTypeRequirement mergeType,
                                        boost::optional<ShardId> mergeShardId)
        : DocumentSource(kStageName, expCtx), _mergeType(mergeType), _mergeShardId(mergeShardId) {}

    Value serialize(const query_shape::SerializationOptions& opts =
                        query_shape::SerializationOptions{}) const final;
    HostTypeRequirement _mergeType = HostTypeRequirement::kNone;

    // Populated with a valid ShardId if this stage was constructed with
    boost::optional<ShardId> _mergeShardId = boost::none;
};

}  // namespace mongo
