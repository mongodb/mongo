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
#include "mongo/db/query/sbe_stage_builder.h"
#include "mongo/db/query/sbe_stage_builder_expression.h"
#include "mongo/db/query/sbe_stage_builder_filter.h"
#include "mongo/db/query/tree_walker.h"
#include "mongo/db/query/util/make_data_structure.h"
#include "mongo/util/overloaded_visitor.h"
#include "mongo/util/str.h"

namespace mongo::stage_builder {
namespace {
using ExpressionType = std::unique_ptr<sbe::EExpression>;

// Enum desribing mode in which projection for the field must be evaluated.
enum class EvalMode {
    // Field should be included in the resulting object with no modification.
    KeepField,
    // Field should be excluded from the resulting object.
    RestrictField,
    // We do not need to do anything with the field (neither exclude nor include).
    IgnoreField,
    // Set field value with an EvalExpr.
    EvaluateField,
};

/**
 * Stores context across calls to visit() in the projection traversal visitors.
 */
struct ProjectionTraversalVisitorContext {
    // Represents current projection level. Created each time visitor encounters path projection.
    struct NestedLevel {
        NestedLevel(StageBuilderState& state,
                    EvalExpr inputExpr,
                    std::list<std::string> fields,
                    boost::optional<sbe::FrameId> lambdaFrame)
            : state(state),
              inputExpr(std::move(inputExpr)),
              fields(std::move(fields)),
              lambdaFrame(std::move(lambdaFrame)) {}

        EvalExpr getInputEvalExpr() const {
            return inputExpr.clone();
        }
        std::unique_ptr<sbe::EExpression> getInputExpr() const {
            return inputExpr.getExpr(state.slotVarMap, *state.data->env);
        }

        EvalExpr extractInputEvalExpr() {
            return std::move(inputExpr);
        }
        std::unique_ptr<sbe::EExpression> extractInputExpr() {
            auto evalExpr = extractInputEvalExpr();
            return evalExpr.extractExpr(state);
        }

        StageBuilderState& state;
        // The input expression for the current level. This is the parent sub-document for each of
        // the projected fields at the current level. 'inputExpr' can be a slot or a local variable.
        EvalExpr inputExpr;
        // The fields names at the current projection level.
        std::list<std::string> fields;
        // The lambda frame associated with the current level.
        boost::optional<sbe::FrameId> lambdaFrame;
        // Vector containing operations for the current level. There are four types of operations
        // (see EvalMode enum for details). The second component of the pair (EvalExpr) is only
        // used for the kEvaluateField operation. For operations other than kEvaluateField, the
        // second component of the pair will be null.
        std::vector<std::pair<EvalMode, EvalExpr>> evals;
        // Whether or not any subtree of this level has a computed field.
        bool subtreeContainsComputedField = false;
    };

    ProjectionTraversalVisitorContext(StageBuilderState& state,
                                      projection_ast::ProjectType projectType,
                                      EvalExpr rootExpr,
                                      const PlanStageSlots* slots)
        : state(state), projectType(projectType), slots(slots) {
        levels.push({state, std::move(rootExpr), {}, boost::none});
    }

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

    void pushKeep() {
        topLevelEvals().emplace_back(EvalMode::KeepField, EvalExpr{});
    }
    void pushRestrict() {
        topLevelEvals().emplace_back(EvalMode::RestrictField, EvalExpr{});
    }
    void pushIgnore() {
        topLevelEvals().emplace_back(EvalMode::IgnoreField, EvalExpr{});
    }
    void pushEvaluate(EvalExpr expr) {
        topLevelEvals().emplace_back(EvalMode::EvaluateField, std::move(expr));
    }

    void popLevel() {
        invariant(!levels.empty());
        invariant(levels.top().fields.empty());
        levels.pop();
    }

    void pushLevel(std::list<std::string> fields,
                   EvalExpr expr,
                   boost::optional<sbe::FrameId> lambdaFrame = boost::none) {
        levels.push({state, std::move(expr), std::move(fields), lambdaFrame});
    }

    EvalExpr done() {
        invariant(levels.size() == 1);
        invariant(topLevelEvals().size() == 1);
        auto mode = topLevelEvals().back().first;
        auto& expr = topLevelEvals().back().second;

        invariant(mode == EvalMode::EvaluateField);

        return std::move(expr);
    }

    StageBuilderState& state;

    projection_ast::ProjectType projectType;

    std::stack<NestedLevel> levels;

    const PlanStageSlots* slots;

    // Flag indicating if $slice operator is used in the projection.
    bool hasSliceProjection = false;

    // Vector containing field names for current field path.
    std::vector<std::string> currentFieldPath;
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
        auto lambdaFrame = boost::make_optional(_context->state.frameId());

        auto expr = EvalExpr{makeVariable(*lambdaFrame, 0)};

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
    const FieldVector& fieldNames, std::vector<std::pair<EvalMode, EvalExpr>>& evals) {
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
        auto mode = evals[i].first;
        auto& expr = evals[i].second;

        switch (mode) {
            case EvalMode::KeepField:
                keepFields.push_back(fieldName);
                break;
            case EvalMode::RestrictField:
                restrictFields.push_back(fieldName);
                break;
            case EvalMode::IgnoreField:
                break;
            case EvalMode::EvaluateField: {
                projectFields.push_back(fieldName);
                projectExprs.emplace_back(std::move(expr));
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
    // Root slot is passed separately from generic context because this class is the only visitor
    // that requires root to be a slot.
    ProjectionTraversalPostVisitor(ProjectionTraversalVisitorContext* context,
                                   sbe::value::SlotId rootSlot)
        : _context{context}, _rootSlot{rootSlot} {}

    void visit(const projection_ast::BooleanConstantASTNode* node) final {
        using namespace std::literals;

        if (node->value()) {
            _context->pushKeep();
        } else {
            _context->pushRestrict();
        }
    }

    void visit(const projection_ast::ExpressionASTNode* node) final {
        // Generate an expression to evaluate a projection expression and push it on top of the
        // 'evals' stack. If the expression is translated into a sub-tree, stack it with the
        // existing sub-tree.
        auto expression = node->expression();
        auto expr =
            generateExpression(_context->state, expression.get(), _rootSlot, _context->slots);

        _context->pushEvaluate(std::move(expr));
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

        auto lambdaFrame = _context->topLevel().lambdaFrame;
        tassert(6897005, "Expected lambda frame to be set", lambdaFrame);

        auto childInputExpr = _context->topLevel().extractInputExpr();

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
            args.push_back(expr.extractExpr(_context->state));
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

        auto fromExpr = [&]() {
            if (_context->isLastLevel()) {
                return _context->topLevel().getInputExpr();
            }
            if (_context->numLevels() == 2 && _context->slots) {
                auto name =
                    std::make_pair(PlanStageSlots::kField, StringData(_context->topFrontField()));
                if (auto slot = _context->slots->getIfExists(name); slot) {
                    return makeVariable(*slot);
                }
            }
            return makeFunction("getField"_sd,
                                _context->topLevel().getInputExpr(),
                                makeConstant(_context->topFrontField()));
        }();

        auto traversePExpr =
            makeFunction("traverseP",
                         std::move(fromExpr),
                         sbe::makeE<sbe::ELocalLambda>(*lambdaFrame, std::move(innerExpr)),
                         makeConstant(sbe::value::TypeTags::Nothing, 0));

        _context->pushEvaluate(std::move(traversePExpr));
    }

    void visit(const projection_ast::ProjectionPositionalASTNode* node) final {
        tasserted(6929402, "Positional projection is not supported in SBE");
    }

    void visit(const projection_ast::ProjectionSliceASTNode* node) final {
        // NOTE: $slice projection operator has it's own path traversal semantics implemented in
        // 'SliceProjectionTraversalPostVisitor'. But before these semantics are applied, path is
        // extracted from the input object according to path traversal semantics of
        // 'BooleanConstantASTNode'. This is why we add 'KeepField' and 'IgnoreField' to evals in
        // this visitor.
        using namespace std::literals;

        if (_context->projectType == projection_ast::ProjectType::kInclusion) {
            _context->pushKeep();
        } else {
            // For exclusion projection we do need to project current field manually, it will be
            // included in the input document anyway.
            _context->pushIgnore();
        }

        _context->hasSliceProjection = true;
    }

    void visit(const projection_ast::ProjectionElemMatchASTNode* node) final {
        tasserted(6929403, "ElemMatch projection is not supported in SBE");
    }

    void visit(const projection_ast::MatchExpressionASTNode* node) final {}

private:
    ProjectionTraversalVisitorContext* _context;
    sbe::value::SlotId _rootSlot;
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
        // other methods of this visitor). This causes the prepareFieldEvals() function to populate
        // 'projectFields' and 'projectExprs' only with evals for $slice operators if there are
        // any. We do not remove any fields in the plan generated by this visitor, so the
        // 'dropFields' and 'keepFields' return values are not used.
        auto [keepFields, dropFields, projectFields, projectExprs] =
            prepareFieldEvals(node->fieldNames(), _context->topLevelEvals());

        tassert(6929404, "Expected 'keepFields' to be empty", keepFields.empty());
        tassert(6929405, "Expected 'dropFields' to be empty", dropFields.empty());

        if (projectExprs.empty()) {
            // Current sub-tree does not contain any $slice operators, so there is no need to change
            // the object. We push an empty eval to match the size of 'evals' vector on the current
            // level with the count of fields.
            _context->popLevel();
            _context->pushIgnore();
            return;
        }

        auto lambdaFrame = _context->topLevel().lambdaFrame;
        tassert(6929406, "Expected lambda frame to be set", lambdaFrame);

        auto childInputExpr = _context->topLevel().extractInputExpr();

        // We've finished extracting what we need from the child level, so pop if off the stack.
        _context->popLevel();

        // Create a makeBsonObj() expression to generate the document for the current nested level.
        // Note that 'dropFields' is empty, so this call to makeBsonObj() will drop no fields and
        // append the computed 'projectFields'.
        auto fieldBehavior = sbe::value::MakeObjSpec::FieldBehavior::drop;
        auto makeObjSpecExpr = makeConstant(
            sbe::value::TypeTags::makeObjSpec,
            sbe::value::bitcastFrom<sbe::value::MakeObjSpec*>(new sbe::value::MakeObjSpec(
                fieldBehavior, std::move(dropFields), std::move(projectFields))));

        auto args = sbe::makeEs(std::move(makeObjSpecExpr), childInputExpr->clone());
        for (auto& expr : projectExprs) {
            args.push_back(expr.extractExpr(_context->state));
        }

        auto innerExpr = sbe::makeE<sbe::EFunction>("makeBsonObj", std::move(args));

        // Anything that is not an object should be preserved as-is.
        innerExpr = sbe::makeE<sbe::EIf>(makeFunction("isObject", childInputExpr->clone()),
                                         std::move(innerExpr),
                                         childInputExpr->clone());

        auto fromExpr = [&]() {
            if (_context->isLastLevel()) {
                return _context->topLevel().getInputExpr();
            }
            if (_context->numLevels() == 2 && _context->slots) {
                auto name =
                    std::make_pair(PlanStageSlots::kField, StringData(_context->topFrontField()));
                if (auto slot = _context->slots->getIfExists(name); slot) {
                    return makeVariable(*slot);
                }
            }
            return makeFunction("getField"_sd,
                                _context->topLevel().getInputExpr(),
                                makeConstant(_context->topFrontField()));
        }();

        // Create the call to traverseP(), going only 1 level in depth (unlike other projection
        // operators which have unlimited depth for the traversal).
        auto traversePExpr =
            makeFunction("traverseP",
                         std::move(fromExpr),
                         sbe::makeE<sbe::ELocalLambda>(*lambdaFrame, std::move(innerExpr)),
                         makeConstant(sbe::value::TypeTags::NumberInt32, 1));

        _context->pushEvaluate(std::move(traversePExpr));
    }

    void visit(const projection_ast::ProjectionPositionalASTNode* node) final {}

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

        _context->pushEvaluate(std::move(sliceExpr));
    }

    void visit(const projection_ast::ProjectionElemMatchASTNode* node) final {}

    void visit(const projection_ast::ExpressionASTNode* node) final {
        // This expression is already built in the 'ProjectionTraversalPostVisitor'. We push an
        // empty eval to match the size of 'evals' vector on the current level with the count of
        // fields.
        _context->pushIgnore();
    }

    void visit(const projection_ast::MatchExpressionASTNode* node) final {}

    void visit(const projection_ast::BooleanConstantASTNode* node) final {
        // This expression is already built in the 'ProjectionTraversalPostVisitor'. We push an
        // empty eval to match the size of 'evals' vector on the current level with the count of
        // fields.
        _context->pushIgnore();
    }

private:
    ProjectionTraversalVisitorContext* _context;
};
}  // namespace

EvalExpr generateProjection(StageBuilderState& state,
                            const projection_ast::Projection* projection,
                            sbe::value::SlotId inputSlot,
                            const PlanStageSlots* slots) {
    auto type = projection->type();
    ProjectionTraversalVisitorContext context{state, type, inputSlot, slots};
    ProjectionTraversalPreVisitor preVisitor{&context};
    ProjectionTraversalInVisitor inVisitor{&context};
    ProjectionTraversalPostVisitor postVisitor{&context, inputSlot};
    projection_ast::ProjectionASTConstWalker walker{&preVisitor, &inVisitor, &postVisitor};
    tree_walker::walk<true, projection_ast::ASTNode>(projection->root(), &walker);

    auto resultExpr = context.done();

    if (!context.hasSliceProjection) {
        return resultExpr;
    }

    auto frameId = state.frameId();
    auto binds = sbe::makeEs(resultExpr.extractExpr(state));
    sbe::EVariable resultRef{frameId, 0};

    // $slice projectional operator has different path traversal semantics compared to other
    // operators. It goes only 1 level in depth when traversing arrays. To keep this semantics
    // we first build a tree to execute all other operators and then build a second tree on top
    // of it for $slice operator. This second tree modifies resulting objects from from other
    // operators to include fields with $slice operator.
    ProjectionTraversalVisitorContext sliceContext{state, type, resultRef.clone(), slots};
    ProjectionTraversalPreVisitor slicePreVisitor{&sliceContext};
    ProjectionTraversalInVisitor sliceInVisitor{&sliceContext};
    SliceProjectionTraversalPostVisitor slicePostVisitor{&sliceContext};
    projection_ast::ProjectionASTConstWalker sliceWalker{
        &slicePreVisitor, &sliceInVisitor, &slicePostVisitor};
    tree_walker::walk<true, projection_ast::ASTNode>(projection->root(), &sliceWalker);

    auto sliceResultExpr = sliceContext.done();

    return sbe::makeE<sbe::ELocalBind>(
        frameId, std::move(binds), sliceResultExpr.extractExpr(state));
}
}  // namespace mongo::stage_builder
