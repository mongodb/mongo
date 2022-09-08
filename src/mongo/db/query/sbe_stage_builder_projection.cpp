/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/query/sbe_stage_builder_projection.h"

#include "mongo/base/exact_cast.h"
#include "mongo/db/exec/sbe/stages/branch.h"
#include "mongo/db/exec/sbe/stages/co_scan.h"
#include "mongo/db/exec/sbe/stages/filter.h"
#include "mongo/db/exec/sbe/stages/limit_skip.h"
#include "mongo/db/exec/sbe/stages/loop_join.h"
#include "mongo/db/exec/sbe/stages/makeobj.h"
#include "mongo/db/exec/sbe/stages/project.h"
#include "mongo/db/exec/sbe/stages/traverse.h"
#include "mongo/db/exec/sbe/stages/union.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/makeobj_spec.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/query/sbe_stage_builder_expression.h"
#include "mongo/db/query/sbe_stage_builder_filter.h"
#include "mongo/db/query/tree_walker.h"
#include "mongo/db/query/util/make_data_structure.h"
#include "mongo/util/overloaded_visitor.h"
#include "mongo/util/str.h"

namespace mongo::stage_builder {
namespace {
using ExpressionType = std::unique_ptr<sbe::EExpression>;
using PlanStageType = EvalStage;

// Enum desribing mode in which projection for the field must be evaluated.
enum class EvalMode {
    // Field should be excluded from the resulting object.
    RestrictField,

    // Field should be included in the resulting object with no modification.
    KeepField,

    // We do not need to do anything with the field (neither exclude nor include).
    IgnoreField,

    // Set field value with an expression or slot from 'ProjectEval' class.
    EvaluateField,
};

// Stores evaluation expressions for each of the projections at the current nested level. 'expr' can
// be nullptr, in this case 'slot' is assigned in 'evalStage' of the current nested level.
class ProjectEval {
public:
    ProjectEval(EvalMode mode) : _mode(mode) {}

    ProjectEval(EvalExpr expr) : _expr{std::move(expr)}, _mode{EvalMode::EvaluateField} {}

    EvalMode mode() const {
        return _mode;
    }

    bool hasSlot() const {
        return _expr.getSlot().has_value();
    }

    sbe::value::SlotId slot() const {
        return *_expr.getSlot();
    }

    ExpressionType extractExpr() {
        return _expr.extractExpr();
    }

    EvalExpr extractEvalExpr() {
        return std::move(_expr);
    }

private:
    EvalExpr _expr;
    EvalMode _mode;
};

struct PositionalProjectionData {
    std::vector<std::string> fieldPath;
    CopyableMatchExpression matchExpression;
};

/**
 * Stores context across calls to visit() in the projection traversal visitors.
 */
struct ProjectionTraversalVisitorContext {
    // Represents current projection level. Created each time visitor encounters path projection.
    struct NestedLevel {
        NestedLevel(EvalExpr inputExpr,
                    std::list<std::string> fields,
                    boost::optional<sbe::FrameId> lambdaFrame)
            : inputExpr(std::move(inputExpr)),
              fields(std::move(fields)),
              lambdaFrame(std::move(lambdaFrame)) {}

        sbe::value::SlotId getInputSlot() const {
            tassert(6897003, "Expected input EvalExpr to be a slot", inputExpr.hasSlot());
            return *inputExpr.getSlot();
        }

        EvalExpr getInputEvalExpr() const {
            return inputExpr.clone();
        }

        ExpressionType getInputExpr() const {
            return getInputEvalExpr().extractExpr();
        }

        // The input expression for the current level. This is the parent sub-document for each of
        // the projected fields at the current level. 'inputExpr' can be a slot or a local variable.
        EvalExpr inputExpr;
        // The fields names at the current projection level.
        std::list<std::string> fields;
        // The lambda frame associated with the current level.
        boost::optional<sbe::FrameId> lambdaFrame;
        // A traversal sub-tree which combines traversals for each of the fields at the current
        // level.
        PlanStageType evalStage;
        // Vector containing expressions for each of the projections at the current level. There is
        // an eval for each of the fields in the current nested level.
        std::vector<ProjectEval> evals;
        // Whether or not any subtree of this level has a computed field.
        bool subtreeContainsComputedField = false;
    };

    const auto& topFrontField() const {
        invariant(!levels.empty());
        invariant(!levels.top().fields.empty());
        return levels.top().fields.front();
    }

    void popFrontField() {
        invariant(!levels.empty());
        invariant(!levels.top().fields.empty());
        levels.top().fields.pop_front();
    }

    size_t numLevels() const {
        return levels.size();
    }

    bool isLastLevel() const {
        return numLevels() == 1;
    }

    auto& topLevel() {
        invariant(!levels.empty());
        return levels.top();
    }

    auto& topLevelEvals() {
        return topLevel().evals;
    }

    void popLevel() {
        invariant(!levels.empty());
        invariant(levels.top().fields.empty());
        levels.pop();
    }

    void pushLevel(std::list<std::string> fields,
                   EvalExpr expr,
                   boost::optional<sbe::FrameId> lambdaFrame = boost::none) {
        levels.push({std::move(expr), std::move(fields), lambdaFrame});
    }

    std::pair<sbe::value::SlotId, PlanStageType> done() {
        invariant(levels.size() == 1);
        auto& evals = topLevelEvals();
        invariant(evals.size() == 1);
        auto& eval = evals[0];

        invariant(eval.mode() == EvalMode::EvaluateField);

        return projectEvalExpr(eval.extractEvalExpr(),
                               std::move(topLevel().evalStage),
                               planNodeId,
                               state.slotIdGenerator);
    }

    ProjectionTraversalVisitorContext(StageBuilderState& state,
                                      PlanNodeId planNodeId,
                                      projection_ast::ProjectType projectType,
                                      PlanStageType inputStage,
                                      sbe::value::SlotId inputSlot,
                                      sbe::value::SlotId preImageSlot,
                                      bool isBasicProjection)
        : state(state),
          planNodeId(planNodeId),
          projectType(projectType),
          inputSlot(inputSlot),
          preImageSlot(preImageSlot),
          isBasicProjection(isBasicProjection) {
        levels.push({inputSlot, {}, boost::none});
        topLevel().evalStage = std::move(inputStage);
    }

    StageBuilderState& state;

    // The node id of the projection QuerySolutionNode.
    const PlanNodeId planNodeId;

    projection_ast::ProjectType projectType;

    // The slot to read a root document from.
    sbe::value::SlotId inputSlot;
    sbe::value::SlotId preImageSlot;

    std::stack<NestedLevel> levels;

    bool isBasicProjection = false;

    // Flag indicating if $slice operator is used in the projection.
    bool hasSliceProjection = false;

    // Vector containing field names for current field path.
    std::vector<std::string> currentFieldPath;

    boost::optional<PositionalProjectionData> positionalProjectionData;
};

/**
 * A projection traversal pre-visitor used for maintaining nested levels while traversing a
 * projection AST.
 */
class ProjectionTraversalPreVisitor final : public projection_ast::ProjectionASTConstVisitor {
public:
    ProjectionTraversalPreVisitor(ProjectionTraversalVisitorContext* context) : _context{context} {
        invariant(_context);
    }

    void visit(const projection_ast::ProjectionPathASTNode* node) final {
        auto isBasicProj = _context->isBasicProjection;
        auto lambdaFrame =
            isBasicProj ? boost::make_optional(_context->state.frameId()) : boost::none;

        auto expr = isBasicProj
            ? EvalExpr{makeVariable(*lambdaFrame, 0)}
            : EvalExpr{_context->numLevels() <= 1 ? _context->inputSlot : _context->state.slotId()};

        _context->pushLevel(
            {node->fieldNames().begin(), node->fieldNames().end()}, std::move(expr), lambdaFrame);

        _context->currentFieldPath.push_back(_context->topFrontField());
    }

    void visit(const projection_ast::ProjectionPositionalASTNode* node) final {}

    void visit(const projection_ast::ProjectionSliceASTNode* node) final {}

    void visit(const projection_ast::ProjectionElemMatchASTNode* node) final {}

    void visit(const projection_ast::ExpressionASTNode* node) final {
        _context->topLevel().subtreeContainsComputedField = true;
    }

    void visit(const projection_ast::MatchExpressionASTNode* node) final {}

    void visit(const projection_ast::BooleanConstantASTNode* node) final {}

private:
    ProjectionTraversalVisitorContext* _context;
};

/**
 * A projection traversal in-visitor used for maintaining nested levels while traversing a
 * projection AST.
 */
class ProjectionTraversalInVisitor final : public projection_ast::ProjectionASTConstVisitor {
public:
    ProjectionTraversalInVisitor(ProjectionTraversalVisitorContext* context) : _context{context} {
        invariant(_context);
    }

    void visit(const projection_ast::ProjectionPathASTNode* node) final {
        _context->popFrontField();
        _context->currentFieldPath.pop_back();
        _context->currentFieldPath.push_back(_context->topFrontField());
    }

    void visit(const projection_ast::ProjectionPositionalASTNode* node) final {}

    void visit(const projection_ast::ProjectionSliceASTNode* node) final {}

    void visit(const projection_ast::ProjectionElemMatchASTNode* node) final {}

    void visit(const projection_ast::ExpressionASTNode* node) final {}

    void visit(const projection_ast::MatchExpressionASTNode* node) final {}

    void visit(const projection_ast::BooleanConstantASTNode* node) final {}

private:
    ProjectionTraversalVisitorContext* _context;
};

namespace {
using FieldVector = std::vector<std::string>;

std::tuple<FieldVector, FieldVector, FieldVector, std::vector<EvalExpr>> prepareFieldEvals(
    const FieldVector& fieldNames, std::vector<ProjectEval>& evals) {
    // Ensure that there is eval for each of the field names.
    invariant(evals.size() == fieldNames.size());

    FieldVector keepFields;
    FieldVector restrictFields;
    FieldVector projectFields;
    std::vector<EvalExpr> projectExprs;

    // Walk through all the fields at the current nested level and,
    //    * For exclusion projections, populate the 'restrictFields' array to be passed to the
    //      mkobj stage, which constructs an output document for the current nested level.
    //    * For inclusion projections, populate the 'keepFields' array to be passed to the
    //      mkobj stage, and also populate the 'projectFields' and 'projectExprs' vectors with
    //      the field names and the projection values (represented as EvalExprs).
    for (size_t i = 0; i < fieldNames.size(); i++) {
        auto& fieldName = fieldNames[i];
        auto& eval = evals[i];

        switch (eval.mode()) {
            case EvalMode::RestrictField:
                restrictFields.push_back(fieldName);
                break;
            case EvalMode::KeepField:
                keepFields.push_back(fieldName);
                break;
            case EvalMode::IgnoreField:
                break;
            case EvalMode::EvaluateField: {
                projectFields.push_back(fieldName);
                projectExprs.emplace_back(eval.extractEvalExpr());
                break;
            }
        }
    }

    return {std::move(keepFields),
            std::move(restrictFields),
            std::move(projectFields),
            std::move(projectExprs)};
}

}  // namespace

/**
 * A projection traversal post-visitor used for maintaining nested levels while traversing a
 * projection AST and producing an SBE traversal sub-tree for each nested level.
 */
class ProjectionTraversalPostVisitor final : public projection_ast::ProjectionASTConstVisitor {
public:
    ProjectionTraversalPostVisitor(ProjectionTraversalVisitorContext* context)
        : _context{context} {}

    void visit(const projection_ast::BooleanConstantASTNode* node) final {
        using namespace std::literals;

        if (node->value()) {
            _context->topLevelEvals().emplace_back(EvalMode::KeepField);
        } else {
            _context->topLevelEvals().emplace_back(EvalMode::RestrictField);
        }
    }

    void visit(const projection_ast::ExpressionASTNode* node) final {
        // Generate an expression to evaluate a projection expression and push it on top of the
        // 'evals' stack. If the expression is translated into a sub-tree, stack it with the
        // existing 'evalStage' sub-tree.
        auto expression = node->expression();
        auto [expr, stage] = generateExpression(_context->state,
                                                expression.get(),
                                                std::move(_context->topLevel().evalStage),
                                                _context->inputSlot,
                                                _context->planNodeId);

        _context->topLevelEvals().emplace_back(std::move(expr));
        _context->topLevel().evalStage = std::move(stage);
    }

    void visit(const projection_ast::ProjectionPathASTNode* node) final {
        using namespace std::literals;

        // Remove the last field name from context and ensure that there are no more left.
        _context->popFrontField();
        _context->currentFieldPath.pop_back();
        invariant(_context->topLevel().fields.empty());

        auto [keepFields, dropFields, projectFields, projectExprs] =
            prepareFieldEvals(node->fieldNames(), _context->topLevelEvals());

        // Generate a document for the current nested level.
        const bool isInclusion = _context->projectType == projection_ast::ProjectType::kInclusion;

        auto [fieldBehavior, fieldVector] = isInclusion
            ? std::make_pair(sbe::value::MakeObjSpec::FieldBehavior::keep, std::move(keepFields))
            : std::make_pair(sbe::value::MakeObjSpec::FieldBehavior::drop, std::move(dropFields));

        auto childLevelStage = std::move(_context->topLevel().evalStage);
        tassert(6897004,
                "Expected inner input stage to be null",
                !_context->isBasicProjection || childLevelStage.stageIsNull());

        auto lambdaFrame = _context->topLevel().lambdaFrame;
        tassert(6897005,
                "Expected lambda frame to be set",
                !_context->isBasicProjection || lambdaFrame);

        auto [childInputSlot, childInputExpr] = [&] {
            auto evalExpr = _context->topLevel().getInputEvalExpr();
            auto slot = evalExpr.getSlot();
            tassert(6897006,
                    "Expected input EvalExpr to be a slot",
                    _context->isBasicProjection || slot);
            return std::make_pair(slot, evalExpr.extractExpr());
        }();

        const bool containsComputedField = _context->topLevel().subtreeContainsComputedField;

        // We've finished extracting what we need from the child level, so pop if off the stack.
        _context->popLevel();

        // If the child's 'subtreeContainsComputedField' flag was set, then propagate it to the
        // parent level.
        _context->topLevel().subtreeContainsComputedField =
            _context->topLevel().subtreeContainsComputedField || containsComputedField;

        // Create a makeBsonObj() expression to generate the document for the current nested level.
        auto makeObjSpecExpr = makeConstant(
            sbe::value::TypeTags::makeObjSpec,
            sbe::value::bitcastFrom<sbe::value::MakeObjSpec*>(new sbe::value::MakeObjSpec(
                fieldBehavior, std::move(fieldVector), std::move(projectFields))));

        auto args = sbe::makeEs(std::move(makeObjSpecExpr), childInputExpr->clone());
        for (auto& expr : projectExprs) {
            args.push_back(expr.extractExpr());
        }

        auto innerExpr = sbe::makeE<sbe::EFunction>("makeBsonObj", std::move(args));

        if (!isInclusion || !containsComputedField) {
            // If this is an inclusion projection and with no computed fields, then anything that's
            // not an object should get filtered out. Example:
            // projection: {a: {b: 1}}
            // document: {a: [1, {b: 2}, 3]}
            // result: {a: [{b: 2}]}
            //
            // If this is an inclusion projection with 1 or more computed fields, then projections
            // of computed fields should always be applied even if the values aren't objects.
            // Example:
            // projection: {a: {b: "x"}}
            // document: {a: [1,2,3]}
            // result: {a: [{b: "x"}, {b: "x"}, {b: "x"}, {b: "x"}]}
            //
            // If this is an exclusion projection, then anything that is not an object should be
            // preserved as-is.
            innerExpr = sbe::makeE<sbe::EIf>(makeFunction("isObject", childInputExpr->clone()),
                                             std::move(innerExpr),
                                             isInclusion && !containsComputedField
                                                 ? makeConstant(sbe::value::TypeTags::Nothing, 0)
                                                 : childInputExpr->clone());
        }

        auto parentInputExpr = _context->topLevel().getInputExpr();
        auto fromExpr = _context->isLastLevel()
            ? std::move(parentInputExpr)
            : makeFunction("getField"_sd,
                           std::move(parentInputExpr),
                           makeConstant(_context->topFrontField()));

        if (_context->isBasicProjection) {
            // If this is a basic projection, we can make use of traverseP().
            auto traversePExpr =
                makeFunction("traverseP",
                             std::move(fromExpr),
                             sbe::makeE<sbe::ELocalLambda>(*lambdaFrame, std::move(innerExpr)),
                             makeConstant(sbe::value::TypeTags::Nothing, 0));

            _context->topLevelEvals().emplace_back(std::move(traversePExpr));

            return;
        }

        // For non-basic projections, childLevelStage may be non-null, so we have to use
        // TraverseStage.
        auto outputSlot = _context->state.slotId();
        auto innerResultSlot = _context->state.slotId();

        auto parentLevelStage = std::move(_context->topLevel().evalStage);

        // Assign 'fromExpr' to childInputSlot. We will use childInputSlot as the input
        // to TraverseStage, and the TraverseStage will feed childInputSlot (or the array
        // elements from childInputSlot if it's an array) as input to innerBranch which
        // evaluates the call to mkBson().
        auto fromBranch = makeProject(std::move(parentLevelStage),
                                      _context->planNodeId,
                                      *childInputSlot,
                                      std::move(fromExpr));

        auto innerBranch = makeProject(std::move(childLevelStage),
                                       _context->planNodeId,
                                       innerResultSlot,
                                       std::move(innerExpr));

        auto traverseStage = makeTraverse(std::move(fromBranch),
                                          std::move(innerBranch),
                                          *childInputSlot,
                                          outputSlot,
                                          innerResultSlot,
                                          nullptr,
                                          nullptr,
                                          _context->planNodeId,
                                          boost::none);

        _context->topLevelEvals().emplace_back(outputSlot);
        _context->topLevel().evalStage = std::move(traverseStage);
    }

    void visit(const projection_ast::ProjectionPositionalASTNode* node) final {
        // NOTE: Positional projection operator has it's own path traversal semantics implemented in
        // 'generatePositionalProjection'. But before these semantics are applied, path is extracted
        // from the input object according to path traversal semantics of 'BooleanConstantASTNode'.
        // This is why we add 'KeepField' to evals in this visitor.
        tassert(5291404,
                "positional projection cannot be used with exclusion",
                _context->projectType == projection_ast::ProjectType::kInclusion);
        _context->topLevelEvals().emplace_back(EvalMode::KeepField);

        const auto& children = node->children();
        invariant(children.size() == 1);
        auto matchExpression =
            exact_pointer_cast<projection_ast::MatchExpressionASTNode*>(children[0].get());
        invariant(matchExpression);
        _context->positionalProjectionData = PositionalProjectionData{
            _context->currentFieldPath, matchExpression->matchExpression()};
    }

    void visit(const projection_ast::ProjectionSliceASTNode* node) final {
        // NOTE: $slice projection operator has it's own path traversal semantics implemented in
        // 'SliceProjectionTraversalPostVisitor'. But before these semantics are applied, path is
        // extracted from the input object according to path traversal semantics of
        // 'BooleanConstantASTNode'. This is why we add 'KeepField' and 'IgnoreField' to evals in
        // this visitor.
        using namespace std::literals;

        auto& evals = _context->topLevelEvals();
        if (_context->projectType == projection_ast::ProjectType::kInclusion) {
            evals.emplace_back(EvalMode::KeepField);
        } else {
            // For exclusion projection we do need to project current field manually, it will be
            // included in the input document anyway.
            evals.emplace_back(EvalMode::IgnoreField);
        }

        _context->hasSliceProjection = true;
    }

    void visit(const projection_ast::ProjectionElemMatchASTNode* node) final {
        using namespace std::literals;

        const auto& children = node->children();
        invariant(children.size() == 1);
        auto matchExpression =
            exact_pointer_cast<projection_ast::MatchExpressionASTNode*>(children[0].get())
                ->matchExpression();

        // We first construct SBE tree for $elemMatch predicate. 'getNext' call for the root of this
        // tree returns ADVANCED if value in the input slot matches the predicate and EOF otherwise.
        //
        // We do not simply pass 'matchExpression' to 'generateFilter' function. Even though
        // 'generateFilter' would generate appropriate tree for the $elemMatch expression, it would
        // not allow us to record which array element matched the predicate.
        // Instead we generate tree for the $elemMatch predicate and put it in the inner branch of
        // traverse stage by ourselves. This allows us to return the first matching element in the
        // array.
        // The $elemMatch projection operator has the same semantics as the $elemMatch match
        // expression, so this code adapts the logic from the 'sbe_stage_builder_filter.cpp'
        // implementation.
        // NOTE: The fact that $elemMatch predicate is passed to 'generateFilter' as root match
        // expression forces function to apply top-level AND optimization. This optimization does
        // not affect the correctness in this case. See 'AndMatchExpression' pre-visitor in
        // 'sbe_stage_builder_filter.cpp' for details.
        auto inputArraySlot = _context->state.slotId();
        auto predicate = [&]() {
            if (matchExpression->matchType() == MatchExpression::ELEM_MATCH_OBJECT) {
                auto elemMatchObject =
                    exact_pointer_cast<const ElemMatchObjectMatchExpression*>(&*matchExpression);
                invariant(elemMatchObject);
                invariant(elemMatchObject->numChildren() == 1);
                auto elemMatchPredicate = elemMatchObject->getChild(0);
                auto [_, elemMatchPredicateTree] = generateFilter(
                    _context->state, elemMatchPredicate, {}, inputArraySlot, _context->planNodeId);

                auto isObjectOrArrayExpr =
                    makeBinaryOp(sbe::EPrimBinary::logicOr,
                                 makeFunction("isObject"_sd, makeVariable(inputArraySlot)),
                                 makeFunction("isArray"_sd, makeVariable(inputArraySlot)));
                return makeFilter<true>(std::move(elemMatchPredicateTree),
                                        std::move(isObjectOrArrayExpr),
                                        _context->planNodeId);
            } else if (matchExpression->matchType() == MatchExpression::ELEM_MATCH_VALUE) {
                auto elemMatchValue =
                    exact_pointer_cast<const ElemMatchValueMatchExpression*>(&*matchExpression);
                invariant(elemMatchValue);
                // 'ElemMatchValueMatchExpression' is an implicit AND operator over its children.
                // Since we cannot pass 'ElemMatchValueMatchExpression' to 'generateFilter' directly
                // we construct an explicit AND operator instead.
                auto topLevelAnd = std::make_unique<AndMatchExpression>();
                for (size_t i = 0; i < elemMatchValue->numChildren(); i++) {
                    auto clonedChild = elemMatchValue->getChild(i)->shallowClone();
                    topLevelAnd->add(std::move(clonedChild));
                }
                auto [_, stage] = generateFilter(
                    _context->state, topLevelAnd.get(), {}, inputArraySlot, _context->planNodeId);
                return std::move(stage);
            } else {
                MONGO_UNREACHABLE;
            }
        }();

        // Predicate's SBE tree is placed in the inner branch of traverse stage. On top of this tree
        // we place project stage to set 'earlyExitFlagSlot' to true. This is needed to stop
        // traversal once we have found first matching array element. To prevent traversal of
        // non-array values we add constant filter stage preventing filter tree from being
        // evaluated.
        //
        // SBE tree looks like this:
        //
        // traverse
        //   arrayToTraverse = inputArraySlot,
        //   currentIterationResult = inputArraySlot,
        //   resultArray = resultArraySlot,
        //   earlyExitCondition = earlyExitFlagSlot
        // from
        //   project traversingAnArrayFlagSlot = isArray(inputArraySlot)
        //   project inputArraySlot = if isObject(inputDocumentSlot)
        //     then
        //       getField(inputDocumentSlot, <field name>)
        //     else
        //       fail()
        //   <current level evalStage>
        // in
        //   cfilter traversingAnArrayFlagSlot
        //   project earlyExitFlagSlot = true
        //   <$elemMatch predicate tree>
        auto earlyExitFlagSlot = _context->state.slotId();
        auto inBranch = makeProject(std::move(predicate),
                                    _context->planNodeId,
                                    earlyExitFlagSlot,
                                    makeConstant(sbe::value::TypeTags::Boolean, true));

        auto traversingAnArrayFlagSlot = _context->state.slotId();
        inBranch = makeFilter<true>(
            std::move(inBranch), makeVariable(traversingAnArrayFlagSlot), _context->planNodeId);

        auto fromBranch = makeProject(std::move(_context->topLevel().evalStage),
                                      _context->planNodeId,
                                      inputArraySlot,
                                      makeFunction("getField"_sd,
                                                   _context->topLevel().getInputExpr(),
                                                   makeConstant(_context->topFrontField())));

        fromBranch = makeProject(std::move(fromBranch),
                                 _context->planNodeId,
                                 traversingAnArrayFlagSlot,
                                 makeFunction("isArray"_sd, makeVariable(inputArraySlot)));

        auto filteredArraySlot = _context->state.slotId();
        auto traverseStage = makeTraverse(std::move(fromBranch),
                                          std::move(inBranch),
                                          inputArraySlot,
                                          filteredArraySlot,
                                          inputArraySlot,
                                          nullptr,
                                          makeVariable(earlyExitFlagSlot),
                                          _context->planNodeId,
                                          1);

        // Finally, we check if the result of traversal is an empty array. In this case, there were
        // no array elements matching the $elemMatch predicate. We replace empty array with Nothing
        // to exclude the field from the resulting object.
        auto resultSlot = _context->state.slotId();
        auto resultStage = makeProject(
            std::move(traverseStage),
            _context->planNodeId,
            resultSlot,
            sbe::makeE<sbe::EIf>(makeFunction("isArrayEmpty", makeVariable(filteredArraySlot)),
                                 makeConstant(sbe::value::TypeTags::Nothing, 0),
                                 makeVariable(filteredArraySlot)));

        _context->topLevelEvals().emplace_back(resultSlot);
        _context->topLevel().evalStage = std::move(resultStage);
    }

    void visit(const projection_ast::MatchExpressionASTNode* node) final {}

private:
    ProjectionTraversalVisitorContext* _context;
};

/**
 * A projection traversal post-visitor used to create separate sub-tree for $slice projectional
 * operator.
 */
class SliceProjectionTraversalPostVisitor final : public projection_ast::ProjectionASTConstVisitor {
public:
    SliceProjectionTraversalPostVisitor(ProjectionTraversalVisitorContext* context)
        : _context{context} {}

    void visit(const projection_ast::ProjectionPathASTNode* node) final {
        using namespace std::literals;

        // Remove the last field name from context and ensure that there are no more left.
        _context->popFrontField();
        _context->currentFieldPath.pop_back();
        invariant(_context->topLevel().fields.empty());

        // All field paths without $slice operator are marked using 'EvalMode::IgnoreField' (see
        // other methods of this visitor). This makes 'prepareFieldEvals' function to populate
        // 'projectFields' and 'projectExprs' only with evals for $slice operators if there are
        // any. We do not remove any fields in the plan generated by this visitor, so the
        // 'restrictFields' and 'keepFields' return values are not used.
        auto [keepFields, restrictFields, projectFields, projectExprs] =
            prepareFieldEvals(node->fieldNames(), _context->topLevelEvals());

        auto childLevelStage{std::move(_context->topLevel().evalStage)};

        sbe::value::SlotMap<ExpressionType> projectsMap;
        sbe::value::SlotVector projectSlots;

        for (auto&& expr : projectExprs) {
            sbe::value::SlotId slot;
            if (!expr.hasSlot()) {
                slot = _context->state.slotId();
                projectsMap.emplace(slot, expr.extractExpr());
            } else {
                slot = *expr.getSlot();
            }

            projectSlots.push_back(slot);
        }

        // If we have something to actually project, then inject a projection stage.
        if (!projectsMap.empty()) {
            childLevelStage = makeProject(
                std::move(childLevelStage), std::move(projectsMap), _context->planNodeId);
        }

        invariant(restrictFields.empty());
        invariant(keepFields.empty());

        if (projectSlots.empty()) {
            // Current sub-tree does not contain any $slice operators, so there is no need to change
            // the object. We push an empty eval to match the size of 'evals' vector on the current
            // level with the count of fields.
            _context->popLevel();
            _context->topLevelEvals().emplace_back(EvalMode::IgnoreField);
            return;
        }

        // Unlike other projectional operators, $slice goes only 1 level in depth for arrays. To
        // implement this logic, we pass 1 as 'nestedArrayDepth' parameter to the traverse stage.
        //
        // Since visitors for $slice operator work on top of the result from other operators, it is
        // important to keep all computed results in the document. To do so, we include branch stage
        // in the inner branch of the traverse stage. This branch allows us to modify existing
        // objects in the traversed array to include results from $slice operator and leave all
        // other array elements unchanged.
        //
        // Tree looks like this:
        //
        // traverse
        //   arrayToTraverse = childLevelInputSlot,
        //   currentIterationResult = childLevelResultSlot,
        //   resultArray = childLevelResultSlot,
        //   nestedArrayDepth = 1
        // from
        //   // This project stage is optional for the last nested level.
        //   project childLevelInputSlot = getField(parentLevelInputSlot, <field name>)
        //   <parentLevelStage>
        // in
        //   branch condition = isObject(childLevelInputSlot), result = childLevelResultSlot
        //   [childLevelObjSlot] then
        //     mkobj output = childLevelObjSlot, root = childLevelInputSlot, fields = ...
        //     <childLevelStage>
        //   [childLevelInputSlot] else
        //     limit 1
        //     coscan
        //
        // Construct mkobj stage which adds fields evaluating $slice operator ('projectFields' and
        // 'projectSlots') to the already constructed object from all previous operators.
        auto childLevelInputSlot = _context->topLevel().getInputSlot();
        auto childLevelObjSlot = _context->state.slotId();
        childLevelStage = makeMkBsonObj(std::move(childLevelStage),
                                        childLevelObjSlot,
                                        childLevelInputSlot,
                                        sbe::MakeBsonObjStage::FieldBehavior::drop,
                                        std::vector<std::string>{},
                                        std::move(projectFields),
                                        std::move(projectSlots),
                                        false,
                                        false,
                                        _context->planNodeId);

        // Create a branch stage which executes mkobj stage if current element in traversal is an
        // object and returns the input unchanged if it has some other type.
        auto childLevelResultSlot = _context->state.slotId();
        childLevelStage = makeBranch(std::move(childLevelStage),
                                     {},
                                     makeFunction("isObject"_sd, makeVariable(childLevelInputSlot)),
                                     sbe::makeSV(childLevelObjSlot),
                                     sbe::makeSV(childLevelInputSlot),
                                     sbe::makeSV(childLevelResultSlot),
                                     _context->planNodeId);

        // We are done with the child level. Now we need to extract corresponding field from parent
        // level, traverse it and assign value to 'childLevelInputSlot'.
        _context->popLevel();

        auto parentLevelInputSlot = _context->topLevel().getInputSlot();
        auto parentLevelStage{std::move(_context->topLevel().evalStage)};
        if (!_context->isLastLevel()) {
            // Extract value of the current field from the object in 'parentLevelInputSlot'.
            parentLevelStage = makeProject(std::move(parentLevelStage),
                                           _context->planNodeId,
                                           childLevelInputSlot,
                                           makeFunction("getField"_sd,
                                                        makeVariable(parentLevelInputSlot),
                                                        makeConstant(_context->topFrontField())));
        } else {
            // For the last nested level input document is simply the whole document we apply
            // projection to.
            invariant(childLevelInputSlot == parentLevelInputSlot);
        }

        // Create the traverse stage, going only 1 level in depth, unlike other projection operators
        // which have unlimited depth for the traversal.
        auto parentLevelResultSlot = _context->state.slotId();
        parentLevelStage = makeTraverse(std::move(parentLevelStage),
                                        std::move(childLevelStage),
                                        childLevelInputSlot,
                                        parentLevelResultSlot,
                                        childLevelResultSlot,
                                        nullptr,
                                        nullptr,
                                        _context->planNodeId,
                                        1 /* nestedArraysDepth */);

        _context->topLevelEvals().emplace_back(parentLevelResultSlot);
        _context->topLevel().evalStage = std::move(parentLevelStage);
    }

    void visit(const projection_ast::ProjectionPositionalASTNode* node) final {
        // This expression is already built in the 'ProjectionTraversalPostVisitor'. We push an
        // empty eval to match the size of 'evals' vector on the current level with the count of
        // fields.
        _context->topLevelEvals().emplace_back(EvalMode::IgnoreField);
    }

    void visit(const projection_ast::ProjectionSliceASTNode* node) final {
        using namespace std::literals;

        auto arrayFromField = makeFunction("getField"_sd,
                                           _context->topLevel().getInputExpr(),
                                           makeConstant(_context->topFrontField()));
        auto binds = sbe::makeEs(std::move(arrayFromField));
        auto frameId = _context->state.frameId();
        sbe::EVariable arrayVariable{frameId, 0};

        auto arguments = sbe::makeEs(
            arrayVariable.clone(), makeConstant(sbe::value::TypeTags::NumberInt32, node->limit()));
        if (node->skip()) {
            invariant(node->limit() >= 0);
            arguments.push_back(makeConstant(sbe::value::TypeTags::NumberInt32, *node->skip()));
        }

        auto extractSubArrayExpr = sbe::makeE<sbe::EIf>(
            makeFunction("isArray"_sd, arrayVariable.clone()),
            sbe::makeE<sbe::EFunction>("extractSubArray", std::move(arguments)),
            arrayVariable.clone());

        auto sliceExpr =
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(extractSubArrayExpr));

        _context->topLevelEvals().emplace_back(std::move(sliceExpr));
    }

    void visit(const projection_ast::ProjectionElemMatchASTNode* node) final {
        // This expression is already built in the 'ProjectionTraversalPostVisitor'. We push an
        // empty eval to match the size of 'evals' vector on the current level with the count of
        // fields.
        _context->topLevelEvals().emplace_back(EvalMode::IgnoreField);
    }

    void visit(const projection_ast::ExpressionASTNode* node) final {
        // This expression is already built in the 'ProjectionTraversalPostVisitor'. We push an
        // empty eval to match the size of 'evals' vector on the current level with the count of
        // fields.
        _context->topLevelEvals().emplace_back(EvalMode::IgnoreField);
    }

    void visit(const projection_ast::MatchExpressionASTNode* node) final {}

    void visit(const projection_ast::BooleanConstantASTNode* node) final {
        // This expression is already built in the 'ProjectionTraversalPostVisitor'. We push an
        // empty eval to match the size of 'evals' vector on the current level with the count of
        // fields.
        _context->topLevelEvals().emplace_back(EvalMode::IgnoreField);
    }

private:
    ProjectionTraversalVisitorContext* _context;
};

/**
 * Generates expression that applies positional projection operator to the array stored in the
 * 'inputSlot' using optional index from 'maybeIndexSlot'.
 * If 'maybeIndexSlot' is boost::none, generates expression that always returns error. Otherwise,
 * generates expression that looks like this:
 *
 *   if isArray(inputSlot) {
 *     if exists(indexSlot) {
 *       let [subArray = extractSubArray(inputArray, 1, indexSlot)]
 *         if isArrayEmpty(subArray) {
 *           fail()
 *         } else {
 *           return subArray
 *         }
 *     } else {
 *       fail()
 *     }
 *   } else {
 *     return Nothing
 *   }
 */
ExpressionType generateApplyPositionalProjectionExpr(
    boost::optional<sbe::value::SlotId> maybeIndexSlot,
    sbe::value::SlotId inputSlot,
    sbe::value::FrameIdGenerator* frameIdGenerator) {
    auto indexIsNotDefinedError = sbe::makeE<sbe::EFail>(
        ErrorCodes::Error{5291401},
        "positional operator '.$' couldn't find a matching element in the array");
    if (!maybeIndexSlot) {
        return indexIsNotDefinedError;
    }

    sbe::EVariable indexVar{*maybeIndexSlot};
    sbe::EVariable inputArray{inputSlot};
    auto subArrayWithElement = makeFunction("extractSubArray",
                                            inputArray.clone(),
                                            makeConstant(sbe::value::TypeTags::NumberInt32, 1),
                                            indexVar.clone());

    auto checkSubArrayEmpty =
        makeLocalBind(frameIdGenerator,
                      [&](sbe::EVariable subArrayWithElement) {
                          return sbe::makeE<sbe::EIf>(
                              makeFunction("isArrayEmpty", subArrayWithElement.clone()),
                              sbe::makeE<sbe::EFail>(ErrorCodes::Error{5291402},
                                                     "positional operator '.$' element mismatch"),
                              subArrayWithElement.clone());
                      },
                      std::move(subArrayWithElement));

    auto checkIndex = sbe::makeE<sbe::EIf>(makeFunction("exists", indexVar.clone()),
                                           std::move(checkSubArrayEmpty),
                                           std::move(indexIsNotDefinedError));

    return sbe::makeE<sbe::EIf>(makeFunction("isArray", inputArray.clone()),
                                std::move(checkIndex),
                                makeConstant(sbe::value::TypeTags::Nothing, 0));
}

/**
 * Generates tree that does path traversal according to positional projection operator semantics.
 */
std::pair<sbe::value::SlotId, PlanStageType> generatePositionalProjection(
    StageBuilderState& state,
    PlanStageType inputStage,
    const PositionalProjectionData& data,
    PlanNodeId planNodeId,
    sbe::value::SlotId postImageSlot,
    sbe::value::SlotId preImageSlot) {
    // First step is to generate filter tree that will record an array index for positional
    // projection.
    auto [maybeIndexSlot, indexStage] = generateFilter(
        state, &*data.matchExpression, {}, preImageSlot, planNodeId, true /* trackIndex */);
    // The index slot is optional because there are certain queries that do not support index
    // tracking (see 'generateFilter' declaration). For such queries we do not want to include
    // stages generated by this function since we will not use any output from them. If index
    // slot is defined, we join 'indexStage' with 'inputStage' using loop-join below. Otherwise,
    // we do not use 'indexStage' at all.
    if (maybeIndexSlot) {
        inputStage = makeLoopJoin(std::move(inputStage), std::move(indexStage), planNodeId);
    }

    // Second step is to implement path traversal semantics for positional projection operator. The
    // general idea is that for each of the components in field path we:
    //  - Extract respective field
    //  - If extracted value is not an object and not an array, we return it unchanged
    //  - If extracted value is an object, we pass it to the next component of the field path
    //  - If extracted value is an array, we apply positional projection operator to it and return
    //    the result
    //
    // For each component there are four main slots:
    //  - 'inputDocumentSlot'. This slot stores document containing current field.
    //  - 'extractedValueSlot'. The value corresponding to the current field is stored in this slot.
    //  - 'nextFieldResultSlot'. This is the result from the next field. If there is a field path
    //    'a.b.c.$' and the current field is 'b', 'nextFieldResultSlot' stores result from
    //    evaluating field 'c'. Note that the loop below goes from field 'c' to field 'a', backwards
    //  - 'currentFieldResultSlot'. This slot stores result from evaluating the current field.
    auto extractedValueSlot = state.slotId();
    sbe::value::SlotId nextFieldResultSlot;
    PlanStageType resultStage;
    const auto& fieldPath = data.fieldPath;
    for (auto it = fieldPath.rbegin(); it != fieldPath.rend(); it++) {
        const auto fieldName = *it;
        // First and last terminology is applied to reading field paths from left to right. In
        // the field path 'a.b.c.$', 'a' is a first field and 'c' is the last one.
        const bool isFirstField = std::next(it) == fieldPath.rend();
        const bool isLastField = it == fieldPath.rbegin();

        sbe::value::SlotId inputDocumentSlot;
        PlanStageType fromBranch;
        if (isFirstField) {
            // For the first field the input document is the post-image document itself.
            inputDocumentSlot = postImageSlot;
            fromBranch = std::move(inputStage);  // NOLINT(bugprone-use-after-move)
        } else {
            // For all other fields input document will be extracted manually.
            inputDocumentSlot = state.slotId();
            fromBranch = {};
        }

        // Construct 'from' branch of the loop-join stage below. Simply extract current field value
        // from the input document.
        fromBranch = makeProject(
            std::move(fromBranch),
            planNodeId,
            extractedValueSlot,
            makeFunction("getField", makeVariable(inputDocumentSlot), makeConstant(fieldName)));

        // Construct 'in' branch of the loop-join stage below. This branch is responsible for what
        // we do with the extracted value: apply positional projection, go deeper into the object
        // or return the value unchanged.
        auto projectionResultSlot = state.slotId();
        auto inBranch =
            makeProject({},
                        planNodeId,
                        projectionResultSlot,
                        generateApplyPositionalProjectionExpr(
                            maybeIndexSlot, extractedValueSlot, state.frameIdGenerator));

        sbe::value::SlotId fieldValueSlot = projectionResultSlot;
        if (!isLastField) {
            // All fields except the last one have the option to pass the extracted value to the
            // next field. Branch stage below checks the type of the extracted value. If it is an
            // array, we apply positional projection operator. Otherwise, we pass the value to the
            // next field.
            invariant(!resultStage.stageIsNull());
            fieldValueSlot = state.slotId();
            inBranch = makeBranch(std::move(inBranch),
                                  std::move(resultStage),
                                  makeFunction("isArray", makeVariable(extractedValueSlot)),
                                  sbe::makeSV(projectionResultSlot),
                                  sbe::makeSV(nextFieldResultSlot),
                                  sbe::makeSV(fieldValueSlot),
                                  planNodeId);
        }

        // After we have computed a new field value (either by applying positional projection or by
        // getting result from the next field), we construct a new object where current field has
        // this new value.
        auto modifiedObjectSlot = state.slotId();
        inBranch = makeMkBsonObj(std::move(inBranch),
                                 modifiedObjectSlot,
                                 inputDocumentSlot,
                                 sbe::MakeBsonObjStage::FieldBehavior::drop,
                                 std::vector<std::string>{},
                                 std::vector<std::string>{fieldName},
                                 sbe::makeSV(fieldValueSlot),
                                 false,
                                 false,
                                 planNodeId);

        // Top branch stage is constructed differently for the last field and others.
        // For the last field, 'inBranch' is containing 'mkobj / project' stages at this point,
        // expecting an array to be stored in 'extractedValueSlot'. This means that top branch must
        // check if 'extractedValueSlot' is actually an array and return the value unchanged
        // otherwise.
        // For all other fields, 'inBranch' is containing 'mkobj / branch / project' stages at this
        // point, expecting an array or object to be stored in 'extractedValueSlot'. In this case,
        // top branch must check if 'extractedValueSlot' is actually an array or object and return
        // the value unchanged otherwise.
        auto applyProjectionCondition =
            makeFunction("isArray", sbe::makeE<sbe::EVariable>(extractedValueSlot));
        if (!isLastField) {
            applyProjectionCondition = sbe::makeE<sbe::EPrimBinary>(
                sbe::EPrimBinary::logicOr,
                std::move(applyProjectionCondition),
                makeFunction("isObject", sbe::makeE<sbe::EVariable>(extractedValueSlot)));
        }

        // We should also check that current field exists in the 'inputDocumentSlot' and return the
        // value unchanged if not.
        applyProjectionCondition = sbe::makeE<sbe::EPrimBinary>(
            sbe::EPrimBinary::logicAnd,
            makeFunction("exists", sbe::makeE<sbe::EVariable>(extractedValueSlot)),
            std::move(applyProjectionCondition));

        // Finally, we construct the top stage of the 'in' branch for the loop-join stage below.
        // This branch stage checks the condition constructed above and returns the
        // 'inputDocumentSlot' unchanged if this condition is false.
        auto currentFieldResultSlot = state.slotId();
        inBranch = makeBranch(std::move(inBranch),
                              {},
                              std::move(applyProjectionCondition),
                              sbe::makeSV(modifiedObjectSlot),
                              sbe::makeSV(inputDocumentSlot),
                              sbe::makeSV(currentFieldResultSlot),
                              planNodeId);

        // Construct the loop-join stage.
        // Final tree for the last field looks like this:
        //
        // nlj correlatedSlots = [extractedValueSlot, inputDocumentSlot]
        // left
        //     project extractedValueSlot = getField(inputDocumentSlot, fieldName)
        //     <limit-1/coscan or stage constructed by 'generateFilter' or 'inputStage'>
        // right
        //     branch
        //         condition = exists(extractedValueSlot) && isArray(extractedValueSlot),
        //         result = currentFieldResultSlot
        //     [modifiedObjectSlot] then
        //         mkbson fieldName = projectionResultSlot
        //         project projectionResultSlot = <position projecton expr>
        //         limit 1
        //         coscan
        //     [inputDocumentSlot] else
        //         limit 1
        //         coscan
        //
        // Final tree for all other fields looks like this:
        //
        // nlj correlatedSlots = [extractedValueSlot, inputDocumentSlot]
        // left
        //     project extractedValueSlot = getField(inputDocumentSlot, fieldName)
        //     <limit-1/coscan or stage constructed by 'generateFilter' or 'inputStage'>
        // right
        //     branch
        //         condition = exists(extractedValueSlot) && isArrayOrObject(extractedValueSlot)
        //         result = currentFieldResultSlot
        //     [modifiedObjectSlot] then
        //         mkbson fieldName = fieldValueSlot
        //         branch condition = isArray(extractedValueSlot)
        //         [projectionResultSlot] then
        //             project projectionResultSlot = <position projecton expr>
        //             limit 1
        //             coscan
        //         [nextFieldResultSlot] else
        //             <resultStage>
        //     [inputDocumentSlot] else
        //         limit 1
        //         coscan
        resultStage = makeLoopJoin(std::move(fromBranch), std::move(inBranch), planNodeId);

        // Exchange slots to hold the invariant. The field on the next iteration is located to the
        // left of the current one, it can be considered previous to the current one. This previous
        // field should extract it's field value into the 'inputDocumentSlot' for the current field.
        // Also, from the previous field perspective current field is the next one, so we should
        // store 'currentFieldResultSlot' in 'nextFieldResultSlot'.
        extractedValueSlot = inputDocumentSlot;
        nextFieldResultSlot = currentFieldResultSlot;
    }

    return {nextFieldResultSlot, std::move(resultStage)};
}

bool projectionIsBasic(const projection_ast::ASTNode* node) {
    if (!node)
        return true;

    auto path = dynamic_cast<const projection_ast::ProjectionPathASTNode*>(node);
    auto boolean = dynamic_cast<const projection_ast::BooleanConstantASTNode*>(node);

    if (!path && !boolean) {
        return false;
    }

    for (size_t i = 0; i < node->children().size(); ++i) {
        if (!projectionIsBasic(node->child(i))) {
            return false;
        }
    }
    return true;
}
}  // namespace

std::pair<sbe::value::SlotId, EvalStage> generateProjection(
    StageBuilderState& state,
    const projection_ast::Projection* projection,
    EvalStage stage,
    sbe::value::SlotId inputVar,
    PlanNodeId planNodeId) {
    auto projType = projection->type();
    bool isBasicProj = projectionIsBasic(projection->root());

    ProjectionTraversalVisitorContext context{
        state, planNodeId, projType, std::move(stage), inputVar, inputVar, isBasicProj};
    ProjectionTraversalPreVisitor preVisitor{&context};
    ProjectionTraversalInVisitor inVisitor{&context};
    ProjectionTraversalPostVisitor postVisitor{&context};
    projection_ast::ProjectionASTConstWalker walker{&preVisitor, &inVisitor, &postVisitor};
    tree_walker::walk<true, projection_ast::ASTNode>(projection->root(), &walker);
    auto [resultSlot, resultStage] = context.done();

    if (context.hasSliceProjection) {
        // $slice projectional operator has different path traversal semantics compared to other
        // operators. It goes only 1 level in depth when traversing arrays. To keep this semantics
        // we first build a tree to execute all other operators and then build a second tree on top
        // of it for $slice operator. This second tree modifies resulting objects from from other
        // operators to include fields with $slice operator.
        ProjectionTraversalVisitorContext sliceContext{
            state, planNodeId, projType, std::move(resultStage), resultSlot, inputVar, isBasicProj};
        ProjectionTraversalPreVisitor slicePreVisitor{&sliceContext};
        ProjectionTraversalInVisitor sliceInVisitor{&sliceContext};
        SliceProjectionTraversalPostVisitor slicePostVisitor{&sliceContext};
        projection_ast::ProjectionASTConstWalker sliceWalker{
            &slicePreVisitor, &sliceInVisitor, &slicePostVisitor};
        tree_walker::walk<true, projection_ast::ASTNode>(projection->root(), &sliceWalker);
        std::tie(resultSlot, resultStage) = sliceContext.done();
    }

    if (context.positionalProjectionData) {
        // Positional projection operator has different path traversal semantics compared to other
        // operators. It goes along the path until it meets an array. Once the array is detected, it
        // extracts the array element using index recorded by query predicate. Path traversal is
        // stopped after this.
        // To implement these semantics we build another tree on top of the existing one. This tree
        // applies positional projection operator to the post-image object.
        // Existing visitor pattern is not suitable for this operator because it has a different
        // evaluation model. Positional projection must be applied to the first array it meets on
        // the path, while other operators are applied only to the leaf path node.
        std::tie(resultSlot, resultStage) =
            generatePositionalProjection(state,
                                         std::move(resultStage),  // NOLINT(bugprone-use-after-move)
                                         *context.positionalProjectionData,
                                         planNodeId,
                                         resultSlot, /* postImageSlot */
                                         inputVar /* preImageSlot */);
    }

    return {resultSlot, std::move(resultStage)};  // NOLINT(bugprone-use-after-move)
}
}  // namespace mongo::stage_builder
