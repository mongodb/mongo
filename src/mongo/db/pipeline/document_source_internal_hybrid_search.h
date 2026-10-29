// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/modules.h"

#include <set>
#include <string_view>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

/**
 * Parse-time DocumentSource for the internal-only $_internalHybridSearch marker that the
 * lite-parse hybrid-search desugarer appends (never at position 0) to the desugared pipeline and
 * each $unionWith sub-pipeline. Execution is a pure passthrough
 * (exec::agg::InternalHybridSearchStage); the stage exists so the marker survives serialization
 * and every collection acquisition re-enforces canRunOnTimeseries=false. The trailing marker is
 * never itself a split point, so when a sharded pipeline splits, PipelineSplitter moves it onto
 * the shards half, where the acquisitions happen.
 */
class DocumentSourceInternalHybridSearch final : public DocumentSource {
public:
    static constexpr std::string_view kStageName = "$_internalHybridSearch"sv;

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement, const boost::intrusive_ptr<ExpressionContext>&);

    DocumentSourceInternalHybridSearch(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSource(kStageName, expCtx) {}

    std::string_view getSourceName() const final {
        return kStageName;
    }

    static const Id& id;

    Id getId() const override {
        return id;
    }

    StageConstraints constraints(PipelineSplitState pipeState) const final {
        // Never legitimately inside $facet or a change stream; error rather than accept.
        StageConstraints constraints(StreamType::kStreaming,
                                     PositionRequirement::kNone,
                                     HostTypeRequirement::kNone,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kNotAllowed,
                                     TransactionRequirement::kAllowed,
                                     LookupRequirement::kAllowed,
                                     UnionRequirement::kAllowed,
                                     ChangeStreamRequirement::kDenylist);
        // Enforced at each collection acquisition via validateWithCollectionMetadata.
        constraints.canRunOnTimeseries = false;
        // A bookkeeping passthrough that should not take part in optimizations...
        constraints.preservesCardinality = false;
        constraints.outputDependsOnSingleInput = false;
        // ...except that $skip/$limit may swap past it: the desugared pipeline ends
        // [..., $sort, marker] and a trailing user $limit must still reach the $sort for top-k.
        constraints.canSwapWithSkippingOrLimitingStage = true;
        return constraints;
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic(
        const DistributedPlanContext* ctx) final {
        return boost::none;
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

private:
    Value serialize(const query_shape::SerializationOptions& opts =
                        query_shape::SerializationOptions{}) const final;
};

}  // namespace mongo
