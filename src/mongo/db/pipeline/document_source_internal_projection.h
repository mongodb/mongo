/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_internal_projection_gen.h"
#include "mongo/db/query/compiler/logical_model/projection/projection.h"

namespace mongo {
/**
 * The 'DocumentSourceInternalProjection' class (internal stage name '$_internalProjection') is used
 * to push aggregation pipeline $project and $addFields stages down to SBE. Both of these are
 * represented by a 'projection_ast::Projection' node since $addFields is implemented as a minor
 * variant of $project.
 */
class DocumentSourceInternalProjection final : public DocumentSource {
public:
    static constexpr StringData kStageNameInternal = "$_internalProjection"_sd;

    DocumentSourceInternalProjection(const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                                     DocumentSourceInternalProjectionSpec spec);

    DocumentSourceInternalProjection(const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                                     BSONObj projectionSpec,
                                     InternalProjectionPolicyEnum policies)
        : DocumentSourceInternalProjection(
              pExpCtx, DocumentSourceInternalProjectionSpec(std::move(projectionSpec), policies)) {}

    const char* getSourceName() const final;

    static const Id& id;

    Id getId() const override {
        return id;
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const final {};

    StageConstraints constraints(PipelineSplitState pipeState) const final {
        StageConstraints constraints(StreamType::kStreaming,
                                     PositionRequirement::kNone,
                                     HostTypeRequirement::kNone,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kAllowed,
                                     TransactionRequirement::kAllowed,
                                     LookupRequirement::kAllowed,
                                     UnionRequirement::kAllowed);

        constraints.canSwapWithMatch = true;
        constraints.canSwapWithSkippingOrLimitingStage = true;
        return constraints;
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final {
        return boost::none;
    }

    DocumentSourceContainer::iterator doOptimizeAt(DocumentSourceContainer::iterator itr,
                                                   DocumentSourceContainer* container) final;


    Value serialize(const SerializationOptions& opts = SerializationOptions{}) const final;

    const projection_ast::Projection& projection() const {
        return _projection;
    }

    const DocumentSourceInternalProjectionSpec& spec() const {
        return _stageSpec;
    }

private:
    // The specification for the $_internalProjection stage (defined by
    // document_source_internal_projection.idl).
    DocumentSourceInternalProjectionSpec _stageSpec;

    // The AST node representing this $project or $addFields.
    projection_ast::Projection _projection;
};
}  // namespace mongo
