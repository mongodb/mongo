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

#include "mongo/db/exec/add_fields_projection_executor.h"
#include "mongo/db/exec/exclusion_projection_executor.h"
#include "mongo/db/exec/inclusion_projection_executor.h"
#include "mongo/db/exec/projection_executor_builder.h"
#include "mongo/db/pipeline/abt/algebrizer_context.h"
#include "mongo/db/pipeline/abt/field_map_builder.h"
#include "mongo/db/pipeline/visitors/transformer_interface_visitor.h"

namespace mongo::optimizer {

class ABTTransformerVisitor : public TransformerInterfaceConstVisitor {
public:
    ABTTransformerVisitor(AlgebrizerContext& ctx, FieldMapBuilder& builder)
        : _ctx(ctx), _builder(builder) {}

    void visit(const projection_executor::AddFieldsProjectionExecutor* transformer) override;

    void visit(const projection_executor::ExclusionProjectionExecutor* transformer) override;

    void visit(const projection_executor::InclusionProjectionExecutor* transformer) override;

    void visit(const GroupFromFirstDocumentTransformation* transformer) override;

    void visit(const ReplaceRootTransformation* transformer) override;

    /**
     * Creates a single EvaluationNode representing simple projections (e.g. inclusion projections)
     * and computed projections, if present, and updates the context with the new node.
     */
    void generateCombinedProjection() const;

private:
    void unsupportedTransformer(const TransformerInterface* transformer) const;

    void assertSupportedPath(const std::string& path);

    /**
     * Handles simple inclusion projections.
     */
    void processProjectedPaths(const projection_executor::InclusionNode& node);

    /**
     * Handles renamed fields and computed projections.
     */
    void processComputedPaths(const projection_executor::InclusionNode& node,
                              const std::string& rootProjection,
                              bool isAddingFields);

    void visitInclusionNode(const projection_executor::InclusionNode& node, bool isAddingFields);

    void visitExclusionNode(const projection_executor::ExclusionNode& node);

    AlgebrizerContext& _ctx;
    FieldMapBuilder& _builder;
};

void translateProjection(AlgebrizerContext& ctx,
                         ProjectionName scanProjName,
                         boost::intrusive_ptr<ExpressionContext> expCtx,
                         const projection_ast::Projection* proj);

}  // namespace mongo::optimizer
