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

#include "mongo/db/exec/sbe/stages/co_scan.h"
#include "mongo/db/exec/sbe/stages/filter.h"
#include "mongo/db/exec/sbe/stages/limit_skip.h"
#include "mongo/db/exec/sbe/stages/makeobj.h"
#include "mongo/db/exec/sbe/stages/project.h"
#include "mongo/db/exec/sbe/stages/traverse.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/query/sbe_stage_builder_expression.h"
#include "mongo/db/query/tree_walker.h"
#include "mongo/util/str.h"
#include "mongo/util/visit_helper.h"

namespace mongo::stage_builder {
namespace {
using ExpressionType = std::unique_ptr<sbe::EExpression>;
using PlanStageType = std::unique_ptr<sbe::PlanStage>;

/**
 * Stores context across calls to visit() in the projection traversal visitors.
 */
struct ProjectionTraversalVisitorContext {
    // Stores the field names to visit at each nested projection level, and the base path to this
    // level. Top of the stack is the most recently visited level.
    struct NestedLevel {
        // The input slot for the current level. This is the parent sub-document for each of the
        // projected fields at the current level.
        sbe::value::SlotId inputSlot;
        // The fields names at the current projection level.
        std::list<std::string> fields;
        // All but the last path component of the current path being visited. None if at the
        // top-level and there is no "parent" path.
        std::stack<std::string> basePath;
        // A traversal sub-tree which combines traversals for each of the fields at the current
        // level.
        PlanStageType fieldPathExpressionsTraverseStage{
            sbe::makeS<sbe::LimitSkipStage>(sbe::makeS<sbe::CoScanStage>(), 1, boost::none)};
    };

    // Stores evaluation expressions for each of the projections at the current nested level. It can
    // evaluate either to an expression, if we're at the leaf node of the projection, or a sub-tree,
    // if we're evaluating a field projection in the middle of the path. The stack elements are
    // optional because for an exclusion projection we don't need to evaluate anything, but we need
    // to have an element on the stack which corresponds to a projected field.
    struct ProjectEval {
        sbe::value::SlotId inputSlot;
        sbe::value::SlotId outputSlot;
        stdx::variant<PlanStageType, ExpressionType> expr;
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

    auto& topLevel() {
        invariant(!levels.empty());
        return levels.top();
    }

    void popLevel() {
        invariant(!levels.empty());
        invariant(levels.top().fields.empty());
        levels.pop();
    }

    void pushLevel(std::list<std::string> fields) {
        levels.push({levels.empty() ? inputSlot : slotIdGenerator->generate(), std::move(fields)});
    }

    std::pair<sbe::value::SlotId, PlanStageType> done() {
        invariant(evals.size() == 1);
        auto eval = std::move(evals.top());
        invariant(eval);
        invariant(stdx::holds_alternative<PlanStageType>(eval->expr));
        return {eval->outputSlot,
                sbe::makeS<sbe::TraverseStage>(std::move(inputStage),
                                               std::move(stdx::get<PlanStageType>(eval->expr)),
                                               eval->inputSlot,
                                               eval->outputSlot,
                                               eval->outputSlot,
                                               sbe::makeSV(),
                                               nullptr,
                                               nullptr)};
    }

    projection_ast::ProjectType projectType;
    sbe::value::SlotIdGenerator* const slotIdGenerator;
    sbe::value::FrameIdGenerator* const frameIdGenerator;

    // The input stage to this projection and the slot to read a root document from.
    PlanStageType inputStage;
    sbe::value::SlotId inputSlot;
    std::stack<NestedLevel> levels;
    std::stack<boost::optional<ProjectEval>> evals;

    // See the comment above the generateExpression() declaration for an explanation of the
    // 'relevantSlots' list.
    sbe::value::SlotVector relevantSlots;
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
        if (node->parent()) {
            _context->topLevel().basePath.push(_context->topFrontField());
            _context->popFrontField();
        }
        _context->pushLevel({node->fieldNames().begin(), node->fieldNames().end()});
    }

    void visit(const projection_ast::ProjectionPositionalASTNode* node) final {
        uasserted(4822885, str::stream() << "Positional projection is not supported in SBE");
    }

    void visit(const projection_ast::ProjectionSliceASTNode* node) final {
        uasserted(4822886, str::stream() << "Slice projection is not supported in SBE");
    }

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
 * A projection traversal post-visitor used for maintaining nested levels while traversing a
 * projection AST and producing an SBE traversal sub-tree for each nested level.
 */
class ProjectionTraversalPostVisitor final : public projection_ast::ProjectionASTConstVisitor {
public:
    ProjectionTraversalPostVisitor(ProjectionTraversalVisitorContext* context) : _context{context} {
        invariant(_context);
    }

    void visit(const projection_ast::BooleanConstantASTNode* node) final {
        using namespace std::literals;

        // If this is an inclusion projection, extract the field and push a getField expression on
        // top of the 'evals' stack. For an exclusion projection just push an empty optional.
        if (node->value()) {
            _context->evals.push(
                {{_context->topLevel().inputSlot,
                  _context->slotIdGenerator->generate(),
                  sbe::makeE<sbe::EFunction>(
                      "getField"sv,
                      sbe::makeEs(sbe::makeE<sbe::EVariable>(_context->topLevel().inputSlot),
                                  sbe::makeE<sbe::EConstant>(_context->topFrontField())))}});
        } else {
            _context->evals.push({});
        }
        _context->popFrontField();
    }

    void visit(const projection_ast::ExpressionASTNode* node) final {
        // Generate an expression to evaluate a projection expression and push it on top of the
        // 'evals' stack. If the expression is translated into a sub-tree, stack it with the
        // existing 'fieldPathExpressionsTraverseStage' sub-tree.
        auto [outputSlot, expr, stage] =
            generateExpression(node->expressionRaw(),
                               std::move(_context->topLevel().fieldPathExpressionsTraverseStage),
                               _context->slotIdGenerator,
                               _context->frameIdGenerator,
                               _context->inputSlot,
                               &_context->relevantSlots);
        _context->evals.push({{_context->topLevel().inputSlot, outputSlot, std::move(expr)}});
        _context->topLevel().fieldPathExpressionsTraverseStage = std::move(stage);
        _context->popFrontField();
    }

    void visit(const projection_ast::ProjectionPathASTNode* node) final {
        using namespace std::literals;

        const auto isInclusion = _context->projectType == projection_ast::ProjectType::kInclusion;
        sbe::value::SlotMap<std::unique_ptr<sbe::EExpression>> projects;
        sbe::value::SlotVector projectSlots;
        std::vector<std::string> projectFields;
        std::vector<std::string> restrictFields;
        auto inputStage{std::move(_context->topLevel().fieldPathExpressionsTraverseStage)};

        invariant(_context->evals.size() >= node->fieldNames().size());
        // Walk through all the fields at the current nested level in reverse order (to match field
        //  names with the elements on the 'evals' stack) and,
        //    * For exclusion projections populate the 'restrictFields' array to be passed to the
        //      mkobj stage below, which constructs an output document for the current nested level.
        //    * For inclusion projections,
        //         - Populates 'projectFields' and 'projectSlots' vectors holding field names to
        //           project, and slots to access evaluated projection values.
        //         - Populates 'projects' map to actually project out the values.
        //         - For nested paths injects a traversal sub-tree.
        for (auto it = node->fieldNames().rbegin(); it != node->fieldNames().rend(); ++it) {
            auto eval = std::move(_context->evals.top());
            _context->evals.pop();

            // If the projection eval element is empty, then this is an exclusion projection and we
            // can put the field name to the vector of restricted fields.
            if (!eval) {
                restrictFields.push_back(*it);
                continue;
            }

            projectSlots.push_back(eval->outputSlot);
            projectFields.push_back(*it);

            stdx::visit(
                visit_helper::Overloaded{
                    [&](ExpressionType& expr) {
                        projects.emplace(eval->outputSlot, std::move(expr));
                    },
                    [&](PlanStageType& stage) {
                        invariant(!_context->topLevel().basePath.empty());

                        inputStage = sbe::makeProjectStage(
                            std::move(inputStage),
                            eval->inputSlot,
                            sbe::makeE<sbe::EFunction>(
                                "getField"sv,
                                sbe::makeEs(
                                    sbe::makeE<sbe::EVariable>(_context->topLevel().inputSlot),
                                    sbe::makeE<sbe::EConstant>(
                                        _context->topLevel().basePath.top()))));
                        _context->topLevel().basePath.pop();

                        inputStage = sbe::makeS<sbe::TraverseStage>(std::move(inputStage),
                                                                    std::move(stage),
                                                                    eval->inputSlot,
                                                                    eval->outputSlot,
                                                                    eval->outputSlot,
                                                                    sbe::makeSV(),
                                                                    nullptr,
                                                                    nullptr);
                    }},
                eval->expr);
        }

        // We walked through the field names in reverse order, so need to reverse the following two
        // vectors.
        std::reverse(projectFields.begin(), projectFields.end());
        std::reverse(projectSlots.begin(), projectSlots.end());

        // If we have something to actually project, then inject a projection stage.
        if (!projects.empty()) {
            inputStage = sbe::makeS<sbe::ProjectStage>(std::move(inputStage), std::move(projects));
        }

        // Finally, inject an mkobj stage to generate a document for the current nested level. For
        // inclusion projection also add constant filter stage on top to filter out input values for
        // nested traversal if they're not documents.
        auto outputSlot = _context->slotIdGenerator->generate();
        _context->relevantSlots.push_back(outputSlot);
        _context->evals.push(
            {{_context->topLevel().inputSlot,
              outputSlot,
              isInclusion ? sbe::makeS<sbe::FilterStage<true>>(
                                sbe::makeS<sbe::MakeObjStage>(std::move(inputStage),
                                                              outputSlot,
                                                              boost::none,
                                                              std::vector<std::string>{},
                                                              std::move(projectFields),
                                                              std::move(projectSlots),
                                                              true,
                                                              false),
                                sbe::makeE<sbe::EFunction>("isObject"sv,
                                                           sbe::makeEs(sbe::makeE<sbe::EVariable>(
                                                               _context->topLevel().inputSlot))))
                          : sbe::makeS<sbe::MakeObjStage>(std::move(inputStage),
                                                          outputSlot,
                                                          _context->topLevel().inputSlot,
                                                          std::move(restrictFields),
                                                          std::move(projectFields),
                                                          std::move(projectSlots),
                                                          false,
                                                          true)}});
        // We've done with the current nested level.
        _context->popLevel();
    }

    void visit(const projection_ast::ProjectionPositionalASTNode* node) final {}
    void visit(const projection_ast::ProjectionSliceASTNode* node) final {}
    void visit(const projection_ast::ProjectionElemMatchASTNode* node) final {}
    void visit(const projection_ast::MatchExpressionASTNode* node) final {}

private:
    ProjectionTraversalVisitorContext* _context;
};

class ProjectionTraversalWalker final {
public:
    ProjectionTraversalWalker(projection_ast::ProjectionASTConstVisitor* preVisitor,
                              projection_ast::ProjectionASTConstVisitor* postVisitor)
        : _preVisitor{preVisitor}, _postVisitor{postVisitor} {}

    void preVisit(const projection_ast::ASTNode* node) {
        node->acceptVisitor(_preVisitor);
    }

    void postVisit(const projection_ast::ASTNode* node) {
        node->acceptVisitor(_postVisitor);
    }

    void inVisit(long count, const projection_ast::ASTNode* node) {}

private:
    projection_ast::ProjectionASTConstVisitor* _preVisitor;
    projection_ast::ProjectionASTConstVisitor* _postVisitor;
};
}  // namespace

std::pair<sbe::value::SlotId, PlanStageType> generateProjection(
    const projection_ast::Projection* projection,
    PlanStageType stage,
    sbe::value::SlotIdGenerator* slotIdGenerator,
    sbe::value::FrameIdGenerator* frameIdGenerator,
    sbe::value::SlotId inputVar) {
    ProjectionTraversalVisitorContext context{
        projection->type(), slotIdGenerator, frameIdGenerator, std::move(stage), inputVar};
    context.relevantSlots.push_back(inputVar);
    ProjectionTraversalPreVisitor preVisitor{&context};
    ProjectionTraversalPostVisitor postVisitor{&context};
    ProjectionTraversalWalker walker{&preVisitor, &postVisitor};
    tree_walker::walk<true, projection_ast::ASTNode>(projection->root(), &walker);
    return context.done();
}
}  // namespace mongo::stage_builder
