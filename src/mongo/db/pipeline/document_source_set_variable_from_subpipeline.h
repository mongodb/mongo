/**
 *    Copyright (C) 2022-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/base/string_data.h"
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

#include <algorithm>
#include <list>
#include <memory>
#include <set>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

struct SetVariableFromSubPipelineSharedState {
    std::unique_ptr<exec::agg::Pipeline> _subExecPipeline;
};
class DocumentSourceSetVariableFromSubPipeline final : public DocumentSource {
public:
    static constexpr StringData kStageName = "$setVariableFromSubPipeline"_sd;

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    static boost::intrusive_ptr<DocumentSourceSetVariableFromSubPipeline> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        std::unique_ptr<Pipeline> subpipeline,
        Variables::Id varID);

    ~DocumentSourceSetVariableFromSubPipeline() override = default;

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final {
        // {shardsStage, mergingStage, sortPattern}
        return DistributedPlanLogic{nullptr, this, boost::none};
    }
    const char* getSourceName() const final {
        return kStageName.data();
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
        setVariableConstraints.preservesOrderAndMetadata = true;
        setVariableConstraints.canSwapWithSkippingOrLimitingStage = true;
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
    Value serialize(const SerializationOptions& opts = SerializationOptions{}) const final;

    const std::shared_ptr<SetVariableFromSubPipelineSharedState> _sharedState;
    std::shared_ptr<Pipeline> _subPipeline;
    Variables::Id _variableID;
};
}  // namespace mongo
