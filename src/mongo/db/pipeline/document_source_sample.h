// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/db/exec/agg/sort_stage.h"
#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline_split_state.h"
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
using namespace std::literals::string_view_literals;

DECLARE_STAGE_PARAMS_DERIVED_DEFAULT(Sample);
class SampleLiteParsed final : public LiteParsedDocumentSourceDefault<SampleLiteParsed> {
public:
    SampleLiteParsed(const BSONElement& originalBson)
        : LiteParsedDocumentSourceDefault<SampleLiteParsed>(originalBson) {}

    static std::unique_ptr<SampleLiteParsed> parse(const NamespaceString& nss,
                                                   const BSONElement& spec,
                                                   const LiteParserOptions& options) {
        return std::make_unique<SampleLiteParsed>(spec);
    }

    std::unique_ptr<StageParams> getStageParams() const final {
        return std::make_unique<SampleStageParams>(_originalBson);
    }

    // $sample only selects a random subset of documents without modifying them.
    bool isSelectionStage() const final {
        return true;
    }
};

class DocumentSourceSample final : public DocumentSource {
public:
    static constexpr std::string_view kStageName = "$sample"sv;

    std::string_view getSourceName() const final {
        return kStageName;
    }

    static const Id& id;

    Id getId() const override {
        return id;
    }

    Value serialize(const query_shape::SerializationOptions& opts =
                        query_shape::SerializationOptions{}) const final;

    StageConstraints constraints(PipelineSplitState pipeState) const final {
        return StageConstraints{StreamType::kBlocking,
                                PositionRequirement::kNone,
                                HostTypeRequirement::kNone,
                                DiskUseRequirement::kWritesTmpData,
                                FacetRequirement::kAllowed,
                                TransactionRequirement::kAllowed,
                                LookupRequirement::kAllowed,
                                UnionRequirement::kAllowed};
    }

    DepsTracker::State getDependencies(DepsTracker* deps) const final {
        deps->needRandomGenerator = true;
        return DepsTracker::State::SEE_NEXT;
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic(
        const DistributedPlanContext* ctx) final;

    long long getSampleSize() const {
        return _size;
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    static boost::intrusive_ptr<DocumentSource> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx, long long size);

private:
    explicit DocumentSourceSample(const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    long long _size;
};
}  // namespace mongo
