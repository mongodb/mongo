// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/util/modules.h"

#include <memory>
#include <set>
#include <string_view>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

DEFINE_LITE_PARSED_STAGE_INTERNAL_DERIVED(ReshardingOwnershipMatch);

/**
 * This is a purpose-built stage to filter out documents which are 'owned' by this shard according
 * to a given shardId and shard key. This stage was created to optimize performance of internal
 * resharding pipelines which need to be able to answer this question very quickly. To do so, it
 * re-uses pieces of sharding infrastructure rather than applying a MatchExpression.
 */
class [[MONGO_MOD_PUBLIC]] DocumentSourceReshardingOwnershipMatch final : public DocumentSource {
public:
    static constexpr std::string_view kStageName = "$_internalReshardingOwnershipMatch"sv;

    static boost::intrusive_ptr<DocumentSourceReshardingOwnershipMatch> create(
        ShardId recipientShardId,
        ShardKeyPattern reshardingKey,
        boost::optional<NamespaceString> temporaryReshardingNamespace,
        const boost::intrusive_ptr<ExpressionContext>& expCtx);

    static boost::intrusive_ptr<DocumentSourceReshardingOwnershipMatch> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    DepsTracker::State getDependencies(DepsTracker* deps) const final;

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

    DocumentSource::GetModPathsReturn getModifiedPaths() const final;

    Value serialize(const query_shape::SerializationOptions& opts =
                        query_shape::SerializationOptions{}) const final;

    StageConstraints constraints(PipelineSplitState pipeState) const final;

    boost::optional<DistributedPlanLogic> distributedPlanLogic(
        const DistributedPlanContext* ctx) final {
        return boost::none;
    }

    std::string_view getSourceName() const final {
        return DocumentSourceReshardingOwnershipMatch::kStageName;
    }

    static const Id& id;

    Id getId() const override {
        return id;
    }

private:
    friend boost::intrusive_ptr<exec::agg::Stage> documentSourceReshardingOwnershipMatchToStageFn(
        const boost::intrusive_ptr<DocumentSource>& documentSource);

    DocumentSourceReshardingOwnershipMatch(
        ShardId recipientShardId,
        ShardKeyPattern reshardingKey,
        boost::optional<NamespaceString> temporaryReshardingNamespace,
        const boost::intrusive_ptr<ExpressionContext>& expCtx);

    const ShardId _recipientShardId;
    // This is a shared_ptr because ReshardingOwnershipMatchStage needs a copy of it too, but it's
    // not copyable.
    std::shared_ptr<ShardKeyPattern> _reshardingKey;
    const boost::optional<NamespaceString> _temporaryReshardingNamespace;
};

}  // namespace mongo
