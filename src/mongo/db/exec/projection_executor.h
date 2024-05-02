/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <boost/intrusive_ptr.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>
#include <memory>
#include <set>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/pipeline/dependencies.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/transformer_interface.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/projection_ast.h"
#include "mongo/db/query/projection_policies.h"
#include "mongo/platform/basic.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

namespace mongo::projection_executor {
/**
 * A ProjectionExecutor is responsible for parsing and executing a $project. It represents either an
 * inclusion or exclusion projection. This is the common interface between the two types of
 * projections.
 */
class ProjectionExecutor : public TransformerInterface {
public:
    /**
     * The name of an internal variable to bind a projection post image to, which is used by the
     * '_rootReplacementExpression' to replace the content of the transformed document.
     */
    static constexpr StringData kProjectionPostImageVarName{"INTERNAL_PROJ_POST_IMAGE"_sd};

    /**
     * Optimize any expressions contained within this projection.
     */
    void optimize() override {
        if (_rootReplacementExpression) {
            _rootReplacementExpression = _rootReplacementExpression->optimize();
        }
    }

    /**
     * Add any dependencies needed by this projection or any sub-expressions to 'deps'.
     */
    DepsTracker::State addDependencies(DepsTracker* deps) const override {
        return DepsTracker::State::NOT_SUPPORTED;
    }

    /**
     * Apply the projection transformation.
     */
    Document applyTransformation(const Document& input) const override {
        auto output = applyProjection(input);
        if (_rootReplacementExpression) {
            return _applyRootReplacementExpression(input, output);
        }
        return output;
    }

    /**
     * Sets 'expr' as a root-replacement expression to this tree. A root-replacement expression,
     * once evaluated, will replace an entire output document. A projection post image document
     * will be accessible via the special variable, whose name is stored in
     * 'kProjectionPostImageVarName', if this expression needs access to it.
     */
    void setRootReplacementExpression(boost::intrusive_ptr<Expression> expr) {
        _rootReplacementExpression = expr;
    }

    /**
     * Returns the root-replacement expression to this tree. Can return nullptr if this tree does
     * not have a root replacing expression.
     */
    boost::intrusive_ptr<Expression> rootReplacementExpression() const {
        return _rootReplacementExpression;
    }

    /**
     * Returns the exhaustive set of all paths that will be preserved by this projection, or
     * boost::none if the exhaustive set cannot be determined.
     */
    virtual boost::optional<std::set<FieldRef>> extractExhaustivePaths() const = 0;

    /**
     * The query shape is made by serializing the first parsed representation of the query, which in
     * the case of $project queries is a projection_ast::Projection. The ProjectionExecutor, holds
     * onto the root node of the AST for only $project queries, so that the first parsed
     * representation is accessible at serialization.
     */
    boost::optional<projection_ast::ProjectionPathASTNode> projection = boost::none;

protected:
    ProjectionExecutor(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                       ProjectionPolicies policies,
                       boost::optional<projection_ast::ProjectionPathASTNode> proj = boost::none)
        : projection(std::move(proj)),
          _expCtx(expCtx),
          _policies(policies),
          _projectionPostImageVarId{
              _expCtx->variablesParseState.defineVariable(kProjectionPostImageVarName)} {}

    /**
     * Apply the projection to 'input'.
     */
    virtual Document applyProjection(const Document& input) const = 0;

    boost::intrusive_ptr<ExpressionContext> _expCtx;

    ProjectionPolicies _policies;

    boost::intrusive_ptr<Expression> _rootReplacementExpression;

private:
    Document _applyRootReplacementExpression(const Document& input, const Document& output) const {
        using namespace fmt::literals;

        _expCtx->variables.setValue(_projectionPostImageVarId, Value{output});
        auto val = _rootReplacementExpression->evaluate(input, &_expCtx->variables);
        uassert(51254,
                "Root-replacement expression must return a document, but got {}"_format(
                    typeName(val.getType())),
                val.getType() == BSONType::Object);
        return val.getDocument();
    }

    // This variable id is used to bind a projection post-image so that it can be accessed by
    // root-replacement expressions which apply projection to the entire post-image document, rather
    // than to a specific field.
    Variables::Id _projectionPostImageVarId;
};
}  // namespace mongo::projection_executor
