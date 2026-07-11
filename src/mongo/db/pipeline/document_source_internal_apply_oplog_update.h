// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
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

DEFINE_LITE_PARSED_STAGE_DEFAULT_DERIVED(InternalApplyOplogUpdate);

/**
 * This is an internal stage that takes an oplog update description and applies the update to the
 * input Document.
 */
class DocumentSourceInternalApplyOplogUpdate final : public DocumentSource {
public:
    static constexpr std::string_view kStageName = "$_internalApplyOplogUpdate"sv;
    static constexpr std::string_view kOplogUpdateFieldName = "oplogUpdate"sv;

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    DocumentSourceInternalApplyOplogUpdate(const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                                           const BSONObj& oplogUpdate);

    std::string_view getSourceName() const override {
        return kStageName;
    }

    static const Id& id;

    Id getId() const override {
        return id;
    }

    StageConstraints constraints(PipelineSplitState pipeState) const override {
        StageConstraints constraints(StreamType::kStreaming,
                                     PositionRequirement::kNone,
                                     HostTypeRequirement::kNone,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kNotAllowed,
                                     TransactionRequirement::kAllowed,
                                     LookupRequirement::kNotAllowed,
                                     UnionRequirement::kNotAllowed,
                                     ChangeStreamRequirement::kDenylist);
        constraints.preservesCardinality = true;
        constraints.canSwapWithMatch = false;
        constraints.canSwapWithSkippingOrLimitingStage = true;
        constraints.isAllowedWithinUpdatePipeline = true;
        constraints.checkExistenceForDiffInsertOperations = true;
        constraints.isIndependentOfAnyCollection = false;
        constraints.consumesLogicalCollectionData = false;
        constraints.outputDependsOnSingleInput = true;
        return constraints;
    }

    DocumentSource::GetModPathsReturn getModifiedPaths() const final {
        return {GetModPathsReturn::Type::kAllPaths, {}, {}};
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic(
        const DistributedPlanContext* ctx) override {
        return boost::none;
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

private:
    friend boost::intrusive_ptr<exec::agg::Stage>
    documentSourceInternalApplyOplogUpdateGroupToStageFn(
        const boost::intrusive_ptr<DocumentSource>&);

    Value serialize(const query_shape::SerializationOptions& opts =
                        query_shape::SerializationOptions{}) const final;

    BSONObj _oplogUpdate;
};

}  // namespace mongo
