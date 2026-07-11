// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/granularity_rounder.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

DEFINE_LITE_PARSED_STAGE_DEFAULT_DERIVED(BucketAuto);

/**
 * The $bucketAuto stage takes a user-specified number of buckets and automatically determines
 * boundaries such that the values are approximately equally distributed between those buckets.
 */
class DocumentSourceBucketAuto final : public DocumentSource {
public:
    static constexpr std::string_view kStageName{"$bucketAuto"};
    Value serialize(const query_shape::SerializationOptions& opts = {}) const final;

    DepsTracker::State getDependencies(DepsTracker* deps) const final;

    void addVariableRefs(std::set<Variables::Id>* refs) const final;

    std::string_view getSourceName() const final;
    boost::intrusive_ptr<DocumentSource> optimize();

    static const Id& id;

    Id getId() const override {
        return id;
    }

    StageConstraints constraints(PipelineSplitState pipeState) const final {
        return {StreamType::kBlocking,
                PositionRequirement::kNone,
                HostTypeRequirement::kNone,
                DiskUseRequirement::kWritesTmpData,
                FacetRequirement::kAllowed,
                TransactionRequirement::kAllowed,
                LookupRequirement::kAllowed,
                UnionRequirement::kAllowed};
    }

    /**
     * The $bucketAuto stage must be run on the merging shard.
     */
    boost::optional<DistributedPlanLogic> distributedPlanLogic(
        const DistributedPlanContext* ctx) final {
        // {shardsStage, mergingStage, sortPattern}
        return DistributedPlanLogic{nullptr, this, boost::none};
    }

    /**
     * Convenience method to create a $bucketAuto stage.
     *
     * If 'accumulationStatements' is the empty vector, it will be filled in with the statement
     * 'count: {$sum: 1}'.
     */
    static boost::intrusive_ptr<DocumentSourceBucketAuto> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const boost::intrusive_ptr<Expression>& groupByExpression,
        int numBuckets,
        std::vector<AccumulationStatement> accumulationStatements = {},
        const boost::intrusive_ptr<GranularityRounder>& granularityRounder = nullptr);

    /**
     * Parses a $bucketAuto stage from the user-supplied BSON.
     */
    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    /**
     * Returns the groupBy expression. The mutable getter can be used to alter
     * the expression, but should not be used after execution has begun.
     */
    boost::intrusive_ptr<Expression> getGroupByExpression() const;
    boost::intrusive_ptr<Expression>& getMutableGroupByExpression();
    /**
     * Returns the AccumulationStatements. The mutable getter can be used to alter
     * the expression, but should not be used after execution has begun.
     */
    const std::vector<AccumulationStatement>& getAccumulationStatements() const;
    std::vector<AccumulationStatement>& getMutableAccumulationStatements();

private:
    friend boost::intrusive_ptr<exec::agg::Stage> documentSourceBucketAutoToStageFn(
        const boost::intrusive_ptr<DocumentSource>& documentSource);

    DocumentSourceBucketAuto(const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                             const boost::intrusive_ptr<Expression>& groupByExpression,
                             int numBuckets,
                             std::vector<AccumulationStatement> accumulationStatements,
                             const boost::intrusive_ptr<GranularityRounder>& granularityRounder);

    std::shared_ptr<std::vector<AccumulationStatement>> _accumulatedFields;

    std::shared_ptr<bool> _populated;
    boost::intrusive_ptr<Expression> _groupByExpression;
    boost::intrusive_ptr<GranularityRounder> _granularityRounder;
    int _nBuckets;
};

}  // namespace mongo
