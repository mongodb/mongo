// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/document_source.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {
using namespace std::literals::string_view_literals;
/**
 * A dummy class for other tests to inherit from to customize the behavior of any of the virtual
 * methods from DocumentSource without having to implement all of them.
 */
class DocumentSourceTestOptimizations : public DocumentSource {
public:
    static constexpr std::string_view kStageName = "$_internalTestOptimizations"sv;
    DocumentSourceTestOptimizations(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSource(DocumentSourceTestOptimizations::kStageName, expCtx) {}
    ~DocumentSourceTestOptimizations() override = default;
    std::string_view getSourceName() const override {
        return DocumentSourceTestOptimizations::kStageName;
    }

    Id getId() const override {
        return kUnallocatedId;
    }

    StageConstraints constraints(PipelineSplitState) const override {
        // Return the default constraints so that this can be used in test pipelines. Constructing a
        // pipeline needs to do some validation that depends on this.
        return StageConstraints{StreamType::kStreaming,
                                PositionRequirement::kNone,
                                HostTypeRequirement::kNone,
                                DiskUseRequirement::kNoDiskUse,
                                FacetRequirement::kAllowed,
                                TransactionRequirement::kNotAllowed,
                                LookupRequirement::kAllowed,
                                UnionRequirement::kAllowed};
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic(
        const DistributedPlanContext* ctx) override {
        return boost::none;
    }

    GetModPathsReturn getModifiedPaths() const override {
        MONGO_UNREACHABLE;
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

private:
    Value serialize(const query_shape::SerializationOptions& opts =
                        query_shape::SerializationOptions{}) const final {
        MONGO_UNREACHABLE_TASSERT(7484301);
    }
};

}  // namespace mongo
