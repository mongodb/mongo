// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/agg/exec_pipeline.h"
#include "mongo/db/exec/agg/pipeline_builder.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/modules.h"

#include <algorithm>
#include <list>
#include <memory>
#include <set>
#include <string_view>
#include <utility>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

DEFINE_LITE_PARSED_STAGE_DEFAULT_DERIVED(SetVariableFromSubPipeline);

struct SetVariableFromSubPipelineSharedState {
    std::unique_ptr<exec::agg::Pipeline> _subExecPipeline;
};
class DocumentSourceSetVariableFromSubPipeline final : public DocumentSource {
public:
    static constexpr std::string_view kStageName = "$setVariableFromSubPipeline"sv;

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    static boost::intrusive_ptr<DocumentSourceSetVariableFromSubPipeline> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        std::unique_ptr<Pipeline> subpipeline,
        Variables::Id varID);

    ~DocumentSourceSetVariableFromSubPipeline() override = default;

    boost::optional<DistributedPlanLogic> distributedPlanLogic(
        const DistributedPlanContext* ctx) final {
        // {shardsStage, mergingStage, sortPattern}
        return DistributedPlanLogic{nullptr, this, boost::none};
    }
    std::string_view getSourceName() const final {
        return kStageName;
    }

    static const Id& id;

    Id getId() const override {
        return id;
    }

    StageConstraints constraints(PipelineSplitState) const final {
        StageConstraints setVariableConstraints(StreamType::kStreaming,
                                                PositionRequirement::kNone,
                                                // Set variable can run anywhere as long as it is
                                                // in the merging half of the pipeline.
                                                HostTypeRequirement::kNone,
                                                DiskUseRequirement::kNoDiskUse,
                                                FacetRequirement::kNotAllowed,
                                                TransactionRequirement::kNotAllowed,
                                                LookupRequirement::kAllowed,
                                                UnionRequirement::kAllowed);

        // The constraints of the sub-pipeline determine the constraints of the
        // $setVariableFromSubPipeline. We want to forward the strictest requirements of the stages
        // in the sub-pipeline.
        if (_subPipeline) {
            setVariableConstraints = StageConstraints::getStrictestConstraints(
                _subPipeline->getSources(), setVariableConstraints);
        }
        // This stage doesn't modify documents.
        setVariableConstraints.preservesCardinality = true;
        setVariableConstraints.preservesOrderAndMetadata = true;
        setVariableConstraints.canSwapWithSkippingOrLimitingStage = true;
        setVariableConstraints.outputDependsOnSingleInput = true;
        return setVariableConstraints;
    }

    std::list<boost::intrusive_ptr<mongo::DocumentSource>>* getSubPipeline() const override {
        return &_subPipeline->getSources();
    }

    auto variableId() const {
        return _variableID;
    }

    DepsTracker::State getDependencies(DepsTracker* deps) const final;

    void addVariableRefs(std::set<Variables::Id>* refs) const final;

    /**
     * Set the sub-pipeline's initial source. Similar to Pipeline's addInitialSource().
     * Should be used to add a cursor/document generating stage to the pipeline.
     */
    void addSubPipelineInitialSource(boost::intrusive_ptr<DocumentSource> source);

    void detachSourceFromOperationContext() final;
    void reattachSourceToOperationContext(OperationContext* opCtx) final;
    bool validateSourceOperationContext(const OperationContext* opCtx) const final;

protected:
    DocumentSourceSetVariableFromSubPipeline(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                             std::unique_ptr<Pipeline> subpipeline,
                                             Variables::Id varID)
        : DocumentSource(kStageName, expCtx),
          _sharedState(std::make_shared<SetVariableFromSubPipelineSharedState>()),
          _subPipeline(std::move(subpipeline)),
          _variableID(varID) {}

private:
    friend boost::intrusive_ptr<exec::agg::Stage> documentSourceSetVariableFromSubPipelineToStageFn(
        const boost::intrusive_ptr<DocumentSource>& documentSource);
    Value serialize(const query_shape::SerializationOptions& opts =
                        query_shape::SerializationOptions{}) const final;

    const std::shared_ptr<SetVariableFromSubPipelineSharedState> _sharedState;
    std::shared_ptr<Pipeline> _subPipeline;
    Variables::Id _variableID;
};
}  // namespace mongo
