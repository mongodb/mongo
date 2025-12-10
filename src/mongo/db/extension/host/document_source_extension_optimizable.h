/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/extension/host/document_source_extension.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/ast_node.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/distributed_plan_logic.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/logical.h"
#include "mongo/util/modules.h"

namespace mongo::extension::host {

class DocumentSourceExtensionOptimizable : public DocumentSourceExtension {
public:
    // Construction of a source or transform stage that expanded from a desugar stage. This stage
    // does not hold a parse node and therefore has no concept of a query shape. Its shape
    // responsibility comes from the desugar stage it expanded from.
    static boost::intrusive_ptr<DocumentSourceExtensionOptimizable> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx, AggStageAstNodeHandle astNode) {
        return boost::intrusive_ptr<DocumentSourceExtensionOptimizable>(
            new DocumentSourceExtensionOptimizable(expCtx, std::move(astNode)));
    }


    /**
     * Construct a DocumentSourceExtensionOptimizable from a logical stage handle.
     *
     * Note: it is important that the input properties match the logical stage type being passed in.
     * Therefore this should only be used when "cloning" an existing document source - e.g. for
     * creating DocumentSources from DPL logical stage handles.
     */
    static boost::intrusive_ptr<DocumentSourceExtensionOptimizable> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        LogicalAggStageHandle logicalStage,
        const MongoExtensionStaticProperties& properties) {
        return boost::intrusive_ptr<DocumentSourceExtensionOptimizable>(
            new DocumentSourceExtensionOptimizable(expCtx, std::move(logicalStage), properties));
    }

    Value serialize(const SerializationOptions& opts) const override;

    StageConstraints constraints(PipelineSplitState pipeState) const override;

    static const Id& id;

    Id getId() const override;

    const MongoExtensionStaticProperties& getStaticProperties() const {
        return _properties;
    }

    DepsTracker::State getDependencies(DepsTracker* deps) const override;

    boost::optional<DistributedPlanLogic> distributedPlanLogic() override;

    // Wrapper around the LogicalAggStageHandle::compile() method. Returns an ExecAggStageHandle.
    ExecAggStageHandle compile() {
        return _logicalStage.compile();
    }

protected:
    const MongoExtensionStaticProperties _properties;
    const LogicalAggStageHandle _logicalStage;

    DocumentSourceExtensionOptimizable(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                       AggStageAstNodeHandle astNode)
        : DocumentSourceExtension(astNode.getName(), expCtx),
          _properties(astNode.getProperties()),
          _logicalStage(astNode.bind()) {}

    DocumentSourceExtensionOptimizable(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                       LogicalAggStageHandle logicalStage,
                                       const MongoExtensionStaticProperties& properties)
        : DocumentSourceExtension(logicalStage.getName(), expCtx),
          _properties(properties),
          _logicalStage(std::move(logicalStage)) {}
};

}  // namespace mongo::extension::host
