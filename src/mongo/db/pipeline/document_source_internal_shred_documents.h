// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/document_source.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {
using namespace std::literals::string_view_literals;

DEFINE_LITE_PARSED_STAGE_DEFAULT_DERIVED(InternalShredDocuments);

/**
 * Converts documents into a shredded format to avoid performance regressions from the switch to
 * having a field cache in 4.4.
 */
class DocumentSourceInternalShredDocuments final : public DocumentSource {
public:
    static constexpr std::string_view kStageName = "$_internalShredDocuments"sv;

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);
    static boost::intrusive_ptr<DocumentSource> create(
        const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    DocumentSourceInternalShredDocuments(const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

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
                                     FacetRequirement::kAllowed,
                                     TransactionRequirement::kAllowed,
                                     LookupRequirement::kAllowed,
                                     UnionRequirement::kAllowed,
                                     ChangeStreamRequirement::kAllowlist);
        constraints.preservesCardinality = true;
        constraints.isAllowedWithinUpdatePipeline = true;
        constraints.outputDependsOnSingleInput = true;
        return constraints;
    }

    Value serialize(const query_shape::SerializationOptions& opts =
                        query_shape::SerializationOptions{}) const override;

    boost::optional<DistributedPlanLogic> distributedPlanLogic(
        const DistributedPlanContext* ctx) override {
        return boost::none;
    }

    DepsTracker::State getDependencies(DepsTracker* deps) const override {
        return DepsTracker::State::SEE_NEXT;
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const override {}
};

}  // namespace mongo
