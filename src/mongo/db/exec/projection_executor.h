// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/transformer_interface.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_policies.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/modules.h"

#include <set>
#include <string_view>

#include <boost/intrusive_ptr.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>

namespace mongo::projection_executor {
using namespace std::literals::string_view_literals;
/**
 * A ProjectionExecutor is responsible for parsing and executing a $project. It represents either an
 * inclusion or exclusion projection. This is the common interface between the two types of
 * projections.
 *
 * TODO SERVER-113179: Remove external dependencies on this class.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] ProjectionExecutor : public TransformerInterface {
public:
    /**
     * The name of an internal variable to bind a projection post image to, which is used by the
     * '_rootReplacementExpression' to replace the content of the transformed document.
     */
    static constexpr std::string_view kProjectionPostImageVarName{"INTERNAL_PROJ_POST_IMAGE"sv};

    /**
     * Optimize any expressions contained within this projection.
     */
    void optimize() override {
        if (_rootReplacementExpression) {
            _rootReplacementExpression = _rootReplacementExpression->optimize();
        }
    }

    DocumentSourceContainer::iterator doOptimizeAt(DocumentSourceContainer::iterator itr,
                                                   DocumentSourceContainer* container) override {
        return std::next(itr);
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
    Document applyTransformation(const Document& input,
                                 const EvaluationContext& ctx) const override {
        auto output = applyProjection(input, ctx);
        if (_rootReplacementExpression) {
            return _applyRootReplacementExpression(input, output, ctx);
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

protected:
    ProjectionExecutor(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                       ProjectionPolicies policies)
        : _expCtx(expCtx),
          _policies(policies),
          _projectionPostImageVarId{
              _expCtx->variablesParseState.defineVariable(kProjectionPostImageVarName)} {}

    /**
     * Apply the projection to 'input'. The 'ctx' parameter carries evaluation state (see
     * EvaluationContext); when it holds a memory tracker, memory usage observed while evaluating
     * any expressions is accumulated against it.
     */
    virtual Document applyProjection(const Document& input, const EvaluationContext& ctx) const = 0;

    boost::intrusive_ptr<ExpressionContext> _expCtx;

    ProjectionPolicies _policies;

    boost::intrusive_ptr<Expression> _rootReplacementExpression;

private:
    Document _applyRootReplacementExpression(const Document& input,
                                             const Document& output,
                                             const EvaluationContext& ctx) const {
        _expCtx->variables.setValue(_projectionPostImageVarId, Value{output});
        auto val = _rootReplacementExpression->evaluate(input, &_expCtx->variables, ctx);
        uassert(51254,
                fmt::format("Root-replacement expression must return a document, but got {}",
                            typeName(val.getType())),
                val.getType() == BSONType::object);
        return val.getDocument();
    }

    // This variable id is used to bind a projection post-image so that it can be accessed by
    // root-replacement expressions which apply projection to the entire post-image document, rather
    // than to a specific field.
    Variables::Id _projectionPostImageVarId;
};
}  // namespace mongo::projection_executor
