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

#include "mongo/db/exec/sbe/stages/branch.h"
#include "mongo/db/exec/sbe/stages/co_scan.h"
#include "mongo/db/exec/sbe/stages/filter.h"
#include "mongo/db/exec/sbe/stages/limit_skip.h"
#include "mongo/db/exec/sbe/stages/makeobj.h"
#include "mongo/db/exec/sbe/stages/project.h"
#include "mongo/db/exec/sbe/stages/traverse.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/query/sbe_stage_builder_expression.h"
#include "mongo/db/query/sbe_stage_builder_helpers.h"
#include "mongo/db/query/tree_walker.h"
#include "mongo/util/str.h"
#include "mongo/util/visit_helper.h"

namespace mongo::stage_builder {
namespace {
using ExpressionType = std::unique_ptr<sbe::EExpression>;
using PlanStageType = std::unique_ptr<sbe::PlanStage>;

// Enum desribing mode in which projection for the field must be evaluated.
enum class EvalMode {
    // Field should be excluded from the resulting object.
    RestrictField,

    // We do not need to do anything with the field (neither exclude nor include).
    IgnoreField,

    // Set field value with an expression or slot from 'ProjectEval' class.
    EvaluateField,
};

// Stores evaluation expressions for each of the projections at the current nested level. 'expr' can
// be nullptr, in this case 'slot' is assigned in 'evalStage' of the current nested level.
class ProjectEval {
public:
    ProjectEval(EvalMode mode) : _slot{sbe::value::SlotId(0)}, _expr{nullptr}, _mode(mode) {}

    ProjectEval(sbe::value::SlotId slot, ExpressionType expr)
        : _slot{slot}, _expr{std::move(expr)}, _mode{EvalMode::EvaluateField} {}

    sbe::value::SlotId slot() const {
        return _slot;
    }

    const ExpressionType& expr() const {
        return _expr;
    }

    EvalMode mode() const {
        return _mode;
    }

    ExpressionType extractExpr() {
        return std::move(_expr);
    }

private:
    sbe::value::SlotId _slot;
    ExpressionType _expr;
    EvalMode _mode;
};

/**
 * Stores context across calls to visit() in the projection traversal visitors.
 */
struct ProjectionTraversalVisitorContext {
    // Represents current projection level. Created each time visitor encounters path projection.
    struct NestedLevel {
        NestedLevel(sbe::value::SlotId inputSlot,
                    std::list<std::string> fields,
                    PlanNodeId planNodeId)
            : inputSlot(inputSlot),
              fields(std::move(fields)),
              evalStage(makeLimitCoScanTree(planNodeId)) {}

        // The input slot for the current level. This is the parent sub-document for each of the
        // projected fields at the current level.
        sbe::value::SlotId inputSlot;
        // The fields names at the current projection level.
        std::list<std::string> fields;
        // A traversal sub-tree which combines traversals for each of the fields at the current
        // level.
        PlanStageType evalStage;
        // Vector containing expressions for each of the projections at the current level. There is
        // an eval for each of the fields in the current nested level.
        std::vector<ProjectEval> evals;
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

    bool isLastLevel() {
        return levels.size() == 1;
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

    void pushLevel(std::list<std::string> fields) {
        levels.push({levels.size() <= 1 ? inputSlot : slotIdGenerator->generate(),
                     std::move(fields),
                     planNodeId});
    }

    std::pair<sbe::value::SlotId, PlanStageType> done() {
        invariant(levels.size() == 1);
        auto& evals = topLevelEvals();
        invariant(evals.size() == 1);
        auto& eval = evals[0];
        invariant(eval.mode() == EvalMode::EvaluateField && !eval.expr());
        return {eval.slot(), std::move(topLevel().evalStage)};
    }

    ProjectionTraversalVisitorContext(OperationContext* opCtx,
                                      PlanNodeId planNodeId,
                                      projection_ast::ProjectType projectType,
                                      sbe::value::SlotIdGenerator* slotIdGenerator,
                                      sbe::value::FrameIdGenerator* frameIdGenerator,
                                      PlanStageType inputStage,
                                      sbe::value::SlotId inputSlot,
                                      sbe::RuntimeEnvironment* env)
        : opCtx(opCtx),
          planNodeId(planNodeId),
          projectType(projectType),
          slotIdGenerator(slotIdGenerator),
          frameIdGenerator(frameIdGenerator),
          inputSlot(inputSlot),
          env(env) {
        pushLevel({});
        topLevel().evalStage = std::move(inputStage);
    }

    OperationContext* opCtx;

    // The node id of the projection QuerySolutionNode.
    const PlanNodeId planNodeId;

    projection_ast::ProjectType projectType;
    sbe::value::SlotIdGenerator* const slotIdGenerator;
    sbe::value::FrameIdGenerator* const frameIdGenerator;

    // The slot to read a root document from.
    sbe::value::SlotId inputSlot;

    sbe::RuntimeEnvironment* env;
    std::stack<NestedLevel> levels;

    // See the comment above the generateExpression() declaration for an explanation of the
    // 'relevantSlots' list.
    sbe::value::SlotVector relevantSlots;

    // Flag indicating if $slice operator is used in the projection.
    bool hasSliceProjection = false;
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
        _context->pushLevel({node->fieldNames().begin(), node->fieldNames().end()});
    }

    void visit(const projection_ast::ProjectionPositionalASTNode* node) final {
        uasserted(4822885, str::stream() << "Positional projection is not supported in SBE");
    }

    void visit(const projection_ast::ProjectionSliceASTNode* node) final {}

    void visit(const projection_ast::ProjectionElemMatchASTNode* node) final {
        uasserted(4822887, str::stream() << "ElemMatch projection is not supported in SBE");
    }

    void visit(const projection_ast::ExpressionASTNode* node) final {}

    void visit(const projection_ast::MatchExpressionASTNode* node) final {
        uasserted(4822888,
                  str::stream() << "Projection match expressions are not supported in SBE");
    }

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

std::tuple<sbe::value::SlotVector, FieldVector, FieldVector, PlanStageType> prepareFieldEvals(
    ProjectionTraversalVisitorContext* context, const projection_ast::ProjectionPathASTNode* node) {
    // Ensure that there is eval for each of the field names.
    auto& evals = context->topLevelEvals();
    const auto& fieldNames = node->fieldNames();
    invariant(evals.size() == fieldNames.size());

    // Walk through all the fields at the current nested level and,
    //    * For exclusion projections populate the 'restrictFields' array to be passed to the
    //      mkobj stage, which constructs an output document for the current nested level.
    //    * For inclusion projections,
    //         - Populates 'projectFields' and 'projectSlots' vectors holding field names to
    //           project, and slots to access evaluated projection values.
    //         - Populates 'projects' map to actually project out the values.
    sbe::value::SlotMap<std::unique_ptr<sbe::EExpression>> projects;
    sbe::value::SlotVector projectSlots;
    std::vector<std::string> projectFields;
    std::vector<std::string> restrictFields;
    for (size_t i = 0; i < fieldNames.size(); i++) {
        auto& fieldName = fieldNames[i];
        auto& eval = evals[i];

        switch (eval.mode()) {
            case EvalMode::IgnoreField:
                // Nothing to do with this field.
                break;
            case EvalMode::RestrictField:
                // This is an exclusion projection and we need put the field name to the vector of
                // restricted fields.
                restrictFields.push_back(fieldName);
                break;
            case EvalMode::EvaluateField: {
                // We need to evaluate value and add a field with it in the resulting object.
                projectSlots.push_back(eval.slot());
                projectFields.push_back(fieldName);

                if (eval.expr()) {
                    projects.emplace(eval.slot(), eval.extractExpr());
                }
                break;
            }
        }
    }

    auto evalStage{std::move(context->topLevel().evalStage)};

    // If we have something to actually project, then inject a projection stage.
    if (!projects.empty()) {
        evalStage = sbe::makeS<sbe::ProjectStage>(
            std::move(evalStage), std::move(projects), context->planNodeId);
    }

    return {std::move(projectSlots),
            std::move(projectFields),
            std::move(restrictFields),
            std::move(evalStage)};
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

        // If this is an inclusion projection, extract the field and push a getField expression on
        // top of the 'evals' stack. For an exclusion projection just push an empty optional.
        if (node->value()) {
            _context->topLevelEvals().emplace_back(
                _context->slotIdGenerator->generate(),
                makeFunction("getField"sv,
                             sbe::makeE<sbe::EVariable>(_context->topLevel().inputSlot),
                             makeConstant(_context->topFrontField())));
        } else {
            _context->topLevelEvals().emplace_back(EvalMode::RestrictField);
        }
    }

    void visit(const projection_ast::ExpressionASTNode* node) final {
        // Generate an expression to evaluate a projection expression and push it on top of the
        // 'evals' stack. If the expression is translated into a sub-tree, stack it with the
        // existing 'evalStage' sub-tree.
        auto [outputSlot, expr, stage] =
            generateExpression(_context->opCtx,
                               node->expression()->optimize().get(),
                               std::move(_context->topLevel().evalStage),
                               _context->slotIdGenerator,
                               _context->frameIdGenerator,
                               _context->inputSlot,
                               _context->env,
                               _context->planNodeId,
                               &_context->relevantSlots);
        _context->topLevelEvals().emplace_back(outputSlot, std::move(expr));
        _context->topLevel().evalStage = std::move(stage);
    }

    void visit(const projection_ast::ProjectionPathASTNode* node) final {
        using namespace std::literals;

        // Remove the last field name from context and ensure that there are no more left.
        _context->popFrontField();
        invariant(_context->topLevel().fields.empty());

        auto [projectSlots, projectFields, restrictFields, childLevelStage] =
            prepareFieldEvals(_context, node);

        // Finally, inject an mkobj stage to generate a document for the current nested level. For
        // inclusion projection also add constant filter stage on top to filter out input values for
        // nested traversal if they're not documents.
        auto childLevelInputSlot = _context->topLevel().inputSlot;
        auto childLevelResultSlot = _context->slotIdGenerator->generate();
        if (_context->projectType == projection_ast::ProjectType::kInclusion) {
            childLevelStage = sbe::makeS<sbe::FilterStage<true>>(
                sbe::makeS<sbe::MakeObjStage>(std::move(childLevelStage),
                                              childLevelResultSlot,
                                              boost::none,
                                              std::vector<std::string>{},
                                              std::move(projectFields),
                                              std::move(projectSlots),
                                              true,
                                              false,
                                              _context->planNodeId),
                makeFunction("isObject"sv, sbe::makeE<sbe::EVariable>(childLevelInputSlot)),
                _context->planNodeId);
        } else {
            childLevelStage = sbe::makeS<sbe::MakeObjStage>(std::move(childLevelStage),
                                                            childLevelResultSlot,
                                                            childLevelInputSlot,
                                                            std::move(restrictFields),
                                                            std::move(projectFields),
                                                            std::move(projectSlots),
                                                            false,
                                                            true,
                                                            _context->planNodeId);
        }

        // We are done with the child level. Now we need to extract corresponding field from parent
        // level, traverse it and assign value to 'childLevelInputSlot'.
        _context->popLevel();

        auto parentLevelInputSlot = _context->topLevel().inputSlot;
        auto parentLevelStage{std::move(_context->topLevel().evalStage)};
        if (!_context->isLastLevel()) {
            parentLevelStage =
                sbe::makeProjectStage(std::move(parentLevelStage),
                                      _context->planNodeId,
                                      childLevelInputSlot,
                                      makeFunction("getField"sv,
                                                   sbe::makeE<sbe::EVariable>(parentLevelInputSlot),
                                                   makeConstant(_context->topFrontField())));
        }

        auto parentLevelResultSlot = _context->slotIdGenerator->generate();
        parentLevelStage = sbe::makeS<sbe::TraverseStage>(std::move(parentLevelStage),
                                                          std::move(childLevelStage),
                                                          childLevelInputSlot,
                                                          parentLevelResultSlot,
                                                          childLevelResultSlot,
                                                          sbe::makeSV(),
                                                          nullptr,
                                                          nullptr,
                                                          _context->planNodeId,
                                                          boost::none);

        _context->topLevel().evalStage = std::move(parentLevelStage);
        _context->topLevelEvals().emplace_back(parentLevelResultSlot, nullptr);
    }

    void visit(const projection_ast::ProjectionPositionalASTNode* node) final {}

    void visit(const projection_ast::ProjectionSliceASTNode* node) final {
        using namespace std::literals;

        auto& evals = _context->topLevelEvals();
        if (_context->projectType == projection_ast::ProjectType::kInclusion) {
            evals.emplace_back(
                _context->slotIdGenerator->generate(),
                makeFunction("getField"sv,
                             sbe::makeE<sbe::EVariable>(_context->topLevel().inputSlot),
                             makeConstant(_context->topFrontField())));
        } else {
            // For exclusion projection we do need to project current field manually, it will be
            // included in the input document anyway.
            evals.emplace_back(EvalMode::IgnoreField);
        }

        _context->hasSliceProjection = true;
    }

    void visit(const projection_ast::ProjectionElemMatchASTNode* node) final {}

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
        invariant(_context->topLevel().fields.empty());

        // All field paths without $slice operator are marked using 'EvalMode::IgnoreField' (see
        // other methods of this visitor). This makes 'prepareFieldEvals' function to populate
        // 'projectSlots' and 'projectFields' only with evals for $slice operators if there are any.
        // We do not restrict any fields in this visitor, so the third returned value is simply
        // ignored.
        auto [projectSlots, projectFields, restrictFields, childLevelStage] =
            prepareFieldEvals(_context, node);
        invariant(restrictFields.empty());

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
        auto childLevelInputSlot = _context->topLevel().inputSlot;
        auto childLevelObjSlot = _context->slotIdGenerator->generate();
        childLevelStage = sbe::makeS<sbe::MakeObjStage>(std::move(childLevelStage),
                                                        childLevelObjSlot,
                                                        childLevelInputSlot,
                                                        std::vector<std::string>{},
                                                        std::move(projectFields),
                                                        std::move(projectSlots),
                                                        false,
                                                        false,
                                                        _context->planNodeId);

        // Create a branch stage which executes mkobj stage if current element in traversal is an
        // object and returns the input unchanged if it has some other type.
        auto childLevelResultSlot = _context->slotIdGenerator->generate();
        childLevelStage = sbe::makeS<sbe::BranchStage>(
            std::move(childLevelStage),
            makeLimitCoScanTree(_context->planNodeId),
            makeFunction("isObject"sv, sbe::makeE<sbe::EVariable>(childLevelInputSlot)),
            sbe::makeSV(childLevelObjSlot),
            sbe::makeSV(childLevelInputSlot),
            sbe::makeSV(childLevelResultSlot),
            _context->planNodeId);

        // We are done with the child level. Now we need to extract corresponding field from parent
        // level, traverse it and assign value to 'childLevelInputSlot'.
        _context->popLevel();

        auto parentLevelInputSlot = _context->topLevel().inputSlot;
        auto parentLevelStage{std::move(_context->topLevel().evalStage)};
        if (!_context->isLastLevel()) {
            // Extract value of the current field from the object in 'parentLevelInputSlot'.
            parentLevelStage =
                sbe::makeProjectStage(std::move(parentLevelStage),
                                      _context->planNodeId,
                                      childLevelInputSlot,
                                      makeFunction("getField"sv,
                                                   sbe::makeE<sbe::EVariable>(parentLevelInputSlot),
                                                   makeConstant(_context->topFrontField())));
        } else {
            // For the last nested level input document is simply the whole document we apply
            // projection to.
            invariant(childLevelInputSlot == parentLevelInputSlot);
        }

        // Create the traverse stage, going only 1 level in depth, unlike other projection operators
        // which have unlimited depth for the traversal.
        auto parentLevelResultSlot = _context->slotIdGenerator->generate();
        parentLevelStage = sbe::makeS<sbe::TraverseStage>(std::move(parentLevelStage),
                                                          std::move(childLevelStage),
                                                          childLevelInputSlot,
                                                          parentLevelResultSlot,
                                                          childLevelResultSlot,
                                                          sbe::makeSV(),
                                                          nullptr,
                                                          nullptr,
                                                          _context->planNodeId,
                                                          1 /* nestedArraysDepth */);

        _context->topLevel().evalStage = std::move(parentLevelStage);
        _context->topLevelEvals().emplace_back(parentLevelResultSlot, nullptr);
    }

    void visit(const projection_ast::ProjectionPositionalASTNode* node) final {}

    void visit(const projection_ast::ProjectionSliceASTNode* node) final {
        using namespace std::literals;

        auto arrayFromField =
            makeFunction("getField"sv,
                         sbe::makeE<sbe::EVariable>(_context->topLevel().inputSlot),
                         makeConstant(_context->topFrontField()));
        auto binds = sbe::makeEs(std::move(arrayFromField));
        auto frameId = _context->frameIdGenerator->generate();
        sbe::EVariable arrayVariable{frameId, 0};

        auto arguments = sbe::makeEs(
            arrayVariable.clone(), makeConstant(sbe::value::TypeTags::NumberInt32, node->limit()));
        if (node->skip()) {
            invariant(node->limit() >= 0);
            arguments.push_back(makeConstant(sbe::value::TypeTags::NumberInt32, *node->skip()));
        }

        auto extractSubArrayExpr = sbe::makeE<sbe::EIf>(
            makeFunction("isArray"sv, arrayVariable.clone()),
            sbe::makeE<sbe::EFunction>("extractSubArray", std::move(arguments)),
            arrayVariable.clone());

        auto sliceExpr =
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(extractSubArrayExpr));

        _context->topLevelEvals().emplace_back(_context->slotIdGenerator->generate(),
                                               std::move(sliceExpr));
    }

    void visit(const projection_ast::ProjectionElemMatchASTNode* node) final {}

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

class ProjectionTraversalWalker final {
public:
    ProjectionTraversalWalker(projection_ast::ProjectionASTConstVisitor* preVisitor,
                              projection_ast::ProjectionASTConstVisitor* inVisitor,
                              projection_ast::ProjectionASTConstVisitor* postVisitor)
        : _preVisitor{preVisitor}, _inVisitor{inVisitor}, _postVisitor{postVisitor} {}

    void preVisit(const projection_ast::ASTNode* node) {
        node->acceptVisitor(_preVisitor);
    }

    void postVisit(const projection_ast::ASTNode* node) {
        node->acceptVisitor(_postVisitor);
    }

    void inVisit(long count, const projection_ast::ASTNode* node) {
        node->acceptVisitor(_inVisitor);
    }

private:
    projection_ast::ProjectionASTConstVisitor* _preVisitor;
    projection_ast::ProjectionASTConstVisitor* _inVisitor;
    projection_ast::ProjectionASTConstVisitor* _postVisitor;
};
}  // namespace

std::pair<sbe::value::SlotId, PlanStageType> generateProjection(
    OperationContext* opCtx,
    const projection_ast::Projection* projection,
    PlanStageType stage,
    sbe::value::SlotIdGenerator* slotIdGenerator,
    sbe::value::FrameIdGenerator* frameIdGenerator,
    sbe::value::SlotId inputVar,
    sbe::RuntimeEnvironment* env,
    PlanNodeId planNodeId) {
    ProjectionTraversalVisitorContext context{opCtx,
                                              planNodeId,
                                              projection->type(),
                                              slotIdGenerator,
                                              frameIdGenerator,
                                              std::move(stage),
                                              inputVar,
                                              env};
    ProjectionTraversalPreVisitor preVisitor{&context};
    ProjectionTraversalInVisitor inVisitor{&context};
    ProjectionTraversalPostVisitor postVisitor{&context};
    ProjectionTraversalWalker walker{&preVisitor, &inVisitor, &postVisitor};
    tree_walker::walk<true, projection_ast::ASTNode>(projection->root(), &walker);
    auto [resultSlot, resultStage] = context.done();

    if (context.hasSliceProjection) {
        // $slice projectional operator has different path traversal semantics compared to other
        // operators. It goes only 1 level in depth when traversing arrays. To keep this semantics
        // we first build a tree to execute all other operators and then build a second tree on top
        // of it for $slice operator. This second tree modifies resulting objects from from other
        // operators to include fields with $slice operator.
        ProjectionTraversalVisitorContext sliceContext{opCtx,
                                                       planNodeId,
                                                       projection->type(),
                                                       slotIdGenerator,
                                                       frameIdGenerator,
                                                       std::move(resultStage),
                                                       resultSlot,
                                                       env};
        ProjectionTraversalPreVisitor slicePreVisitor{&sliceContext};
        ProjectionTraversalInVisitor sliceInVisitor{&sliceContext};
        SliceProjectionTraversalPostVisitor slicePostVisitor{&sliceContext};
        ProjectionTraversalWalker sliceWalker{&slicePreVisitor, &sliceInVisitor, &slicePostVisitor};
        tree_walker::walk<true, projection_ast::ASTNode>(projection->root(), &sliceWalker);
        std::tie(resultSlot, resultStage) = sliceContext.done();
    }

    return {resultSlot, std::move(resultStage)};
}
}  // namespace mongo::stage_builder
