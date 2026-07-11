// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/query_shape/serialization_options.h"

#include <set>
#include <string_view>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

DECLARE_STAGE_PARAMS_DERIVED_DEFAULT(InternalAssertDataAssumptions);
class InternalAssertDataAssumptionsLiteParsed final
    : public LiteParsedDocumentSourceDefault<InternalAssertDataAssumptionsLiteParsed> {
public:
    InternalAssertDataAssumptionsLiteParsed(const BSONElement& originalBson)
        : LiteParsedDocumentSourceDefault<InternalAssertDataAssumptionsLiteParsed>(originalBson) {}

    static std::unique_ptr<InternalAssertDataAssumptionsLiteParsed> parse(
        const NamespaceString& nss, const BSONElement& spec, const LiteParserOptions& options) {
        return std::make_unique<InternalAssertDataAssumptionsLiteParsed>(spec);
    }

    std::unique_ptr<StageParams> getStageParams() const final {
        return std::make_unique<InternalAssertDataAssumptionsStageParams>(_originalBson);
    }

    bool isSelectionStage() const final {
        return true;
    }
};

/**
 * $_internalAssertDataAssumptions is a test-only internal stage that validates the dependency
 * graph's arrayness analysis. It accepts a set of FieldPaths and for each input document, asserts
 * that none of those fields contain arrays. This stage is used in conjunction with the
 * internalEnableDependencyGraphValidation query knob and is automatically inserted by a pipeline
 * rewrite pass before stages where the dependency graph reports canPathBeArray() == false for
 * certain fields.
 *
 * The purpose of this stage is to catch bugs in the dependency graph's arrayness tracking by
 * validating at runtime that fields claimed to be non-array actually do not contain arrays.
 *
 * This is a passthrough stage that does not modify documents, it only validates them.
 */
class DocumentSourceInternalAssertDataAssumptions final : public DocumentSource {
public:
    static constexpr std::string_view kStageName = "$_internalAssertDataAssumptions"sv;

    /**
     * Creates a DocumentSourceInternalAssertDataAssumptions from a BSONElement specification.
     */
    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    /**
     * Creates a DocumentSourceInternalAssertDataAssumptions with the given set of field paths
     * that must not contain arrays.
     */
    static boost::intrusive_ptr<DocumentSourceInternalAssertDataAssumptions> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx, std::set<FieldPath> nonArrayPaths);

    DocumentSourceInternalAssertDataAssumptions(
        const boost::intrusive_ptr<ExpressionContext>& expCtx, std::set<FieldPath> nonArrayPaths);

    std::string_view getSourceName() const final {
        return kStageName;
    }

    DepsTracker::State getDependencies(DepsTracker* deps) const final;

    static const Id& id;

    Id getId() const final {
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
                                     UnionRequirement::kAllowed);
        constraints.canRunOnTimeseries = true;
        return constraints;
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic(
        const DistributedPlanContext* ctx) final {
        return boost::none;
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}
    Value serialize(const query_shape::SerializationOptions& opts =
                        query_shape::SerializationOptions{}) const final;

    const std::set<FieldPath>& getNonArrayPaths() const {
        return _nonArrayPaths;
    }

private:
    // Set of field paths that must not contain arrays in any document
    std::set<FieldPath> _nonArrayPaths;
};

}  // namespace mongo
