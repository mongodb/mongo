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

DECLARE_STAGE_PARAMS_DERIVED_DEFAULT(InternalInhibitOptimization);
class InternalInhibitOptimizationLiteParsed final
    : public LiteParsedDocumentSourceDefault<InternalInhibitOptimizationLiteParsed> {
public:
    InternalInhibitOptimizationLiteParsed(const BSONElement& originalBson)
        : LiteParsedDocumentSourceDefault<InternalInhibitOptimizationLiteParsed>(originalBson) {}

    static std::unique_ptr<InternalInhibitOptimizationLiteParsed> parse(
        const NamespaceString& nss, const BSONElement& spec, const LiteParserOptions& options) {
        return std::make_unique<InternalInhibitOptimizationLiteParsed>(spec);
    }

    std::unique_ptr<StageParams> getStageParams() const final {
        return std::make_unique<InternalInhibitOptimizationStageParams>(_originalBson);
    }

    // $_internalInhibitOptimization is a passthrough that does not modify documents.
    bool isSelectionStage() const final {
        return true;
    }
};

/**
 * An internal stage available for testing. Acts as a simple passthrough of intermediate results
 * from the source stage. Does not participate in optimizations such as swapping, coalescing, or
 * pushdown into the query system, so this stage can be useful in tests to ensure that an
 * unoptimized code path is being exercised.
 */
class DocumentSourceInternalInhibitOptimization final : public DocumentSource {
public:
    static constexpr std::string_view kStageName = "$_internalInhibitOptimization"sv;

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement, const boost::intrusive_ptr<ExpressionContext>&);

    DocumentSourceInternalInhibitOptimization(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSource(kStageName, expCtx) {}

    std::string_view getSourceName() const final {
        return kStageName;
    }

    static const Id& id;

    Id getId() const override {
        return id;
    }

    StageConstraints constraints(PipelineSplitState pipeState) const final {
        StageConstraints constraints(StreamType::kStreaming,
                                     PositionRequirement::kNone,
                                     HostTypeRequirement::kNone,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kAllowed,
                                     TransactionRequirement::kAllowed,
                                     LookupRequirement::kAllowed,
                                     UnionRequirement::kAllowed,
                                     ChangeStreamRequirement::kAllowlist);
        // Given that InternalInhibit stage despite being a passthrough stage, should not take part
        // in any optimization, we explicitely set that it doesn't preserve cardinality.
        constraints.preservesCardinality = false;
        constraints.outputDependsOnSingleInput = false;
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
