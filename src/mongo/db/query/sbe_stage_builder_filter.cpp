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

#include "mongo/db/query/sbe_stage_builder_filter.h"

#include "mongo/db/exec/sbe/stages/co_scan.h"
#include "mongo/db/exec/sbe/stages/filter.h"
#include "mongo/db/exec/sbe/stages/limit_skip.h"
#include "mongo/db/exec/sbe/stages/loop_join.h"
#include "mongo/db/exec/sbe/stages/project.h"
#include "mongo/db/exec/sbe/stages/traverse.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_expr.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/matcher/expression_internal_expr_eq.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_text.h"
#include "mongo/db/matcher/expression_text_noop.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/matcher/expression_type.h"
#include "mongo/db/matcher/expression_visitor.h"
#include "mongo/db/matcher/expression_where.h"
#include "mongo/db/matcher/expression_where_noop.h"
#include "mongo/db/matcher/match_expression_walker.h"
#include "mongo/db/matcher/schema/expression_internal_schema_all_elem_match_from_index.h"
#include "mongo/db/matcher/schema/expression_internal_schema_allowed_properties.h"
#include "mongo/db/matcher/schema/expression_internal_schema_cond.h"
#include "mongo/db/matcher/schema/expression_internal_schema_eq.h"
#include "mongo/db/matcher/schema/expression_internal_schema_fmod.h"
#include "mongo/db/matcher/schema/expression_internal_schema_match_array_index.h"
#include "mongo/db/matcher/schema/expression_internal_schema_max_items.h"
#include "mongo/db/matcher/schema/expression_internal_schema_max_length.h"
#include "mongo/db/matcher/schema/expression_internal_schema_max_properties.h"
#include "mongo/db/matcher/schema/expression_internal_schema_min_items.h"
#include "mongo/db/matcher/schema/expression_internal_schema_min_length.h"
#include "mongo/db/matcher/schema/expression_internal_schema_min_properties.h"
#include "mongo/db/matcher/schema/expression_internal_schema_object_match.h"
#include "mongo/db/matcher/schema/expression_internal_schema_root_doc_eq.h"
#include "mongo/db/matcher/schema/expression_internal_schema_unique_items.h"
#include "mongo/db/matcher/schema/expression_internal_schema_xor.h"
#include "mongo/db/query/sbe_stage_builder_expression.h"
#include "mongo/util/str.h"

namespace mongo::stage_builder {
namespace {
/**
 * The various flavors of PathMatchExpressions require the same skeleton of traverse operators in
 * order to perform implicit path traversal, but may translate differently to an SBE expression that
 * actually applies the predicate against an individual array element.
 *
 * A function of this type can be called to generate an EExpression which applies a predicate to the
 * value found in 'inputSlot'.
 */
using MakePredicateEExprFn =
    std::function<std::unique_ptr<sbe::EExpression>(sbe::value::SlotId inputSlot)>;

/**
 * A struct for storing context across calls to visit() methods in MatchExpressionVisitor's.
 */
struct MatchExpressionVisitorContext {
    MatchExpressionVisitorContext(sbe::value::SlotIdGenerator* slotIdGenerator,
                                  std::unique_ptr<sbe::PlanStage> inputStage,
                                  sbe::value::SlotId inputVar)
        : slotIdGenerator{slotIdGenerator}, inputStage{std::move(inputStage)}, inputVar{inputVar} {}

    std::unique_ptr<sbe::PlanStage> done() {
        if (!predicateVars.empty()) {
            invariant(predicateVars.size() == 1);
            inputStage = sbe::makeS<sbe::FilterStage<false>>(
                std::move(inputStage), sbe::makeE<sbe::EVariable>(predicateVars.top()));
            predicateVars.pop();
        }
        return std::move(inputStage);
    }

    sbe::value::SlotIdGenerator* slotIdGenerator;
    std::unique_ptr<sbe::PlanStage> inputStage;
    std::stack<sbe::value::SlotId> predicateVars;
    std::stack<std::pair<const MatchExpression*, size_t>> nestedLogicalExprs;
    sbe::value::SlotId inputVar;
};

std::unique_ptr<sbe::PlanStage> makeLimitCoScanTree(long long limit = 1) {
    return sbe::makeS<sbe::LimitSkipStage>(sbe::makeS<sbe::CoScanStage>(), limit, boost::none);
}

std::unique_ptr<sbe::EExpression> makeFillEmptyFalse(std::unique_ptr<sbe::EExpression> e) {
    using namespace std::literals;
    return sbe::makeE<sbe::EFunction>(
        "fillEmpty"sv,
        sbe::makeEs(std::move(e), sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Boolean, 0)));
}

/**
 * Helper to check if the current comparison expression is a branch of a logical $and expression.
 * If it is but is not the last one, inject a filter stage to bail out early from the $and predicate
 * without the need to evaluate all branches. If this is the last branch of the $and expression, or
 * if it's not within a logical expression at all, just keep the predicate var on the top on the
 * stack and let the parent expression process it.
 */
void checkForShortCircuitFromLogicalAnd(MatchExpressionVisitorContext* context) {
    if (!context->nestedLogicalExprs.empty() && context->nestedLogicalExprs.top().second > 1 &&
        context->nestedLogicalExprs.top().first->matchType() == MatchExpression::AND) {
        context->inputStage = sbe::makeS<sbe::FilterStage<false>>(
            std::move(context->inputStage),
            sbe::makeE<sbe::EVariable>(context->predicateVars.top()));
        context->predicateVars.pop();
    }
}

enum class LeafArrayTraversalMode {
    kArrayAndItsElements = 0,  // Visit both the array's elements _and_ the array itself
    kArrayElementsOnly = 1,    // Visit the array's elements but don't visit the array itself
};

/**
 * A helper function to generate a path traversal plan stage at the given nested 'level' of the
 * traversal path. For example, for a dotted path expression {'a.b': 2}, the traversal sub-tree will
 * look like this:
 *
 *     traverse
 *          outputVar1 // the traversal result
 *          innerVar1 // the result coming from the 'in' branch
 *          fieldVar1 // field 'a' projected in the 'from' branch, this is the field we will be
 *                    // traversing
 *          {outputVar1 || innerVar1} // the folding expression - combining
 *                                    // results for each element
 *          {outputVar1} // final (early out) expression - when we hit the 'true' value,
 *                       // we don't have to traverse the whole array
 *      in
 *          project [innerVar1 =                               // if getField(fieldVar1,'b') returns
 *                    fillEmpty(outputVar2, false) ||          // an array, compare the array itself
 *                    (fillEmpty(isArray(fieldVar), false) &&  // to 2 as well
 *                     fillEmpty(fieldVar2==2, false))]
 *          traverse // nested traversal
 *              outputVar2 // the traversal result
 *              innerVar2 // the result coming from the 'in' branch
 *              fieldVar2 // field 'b' projected in the 'from' branch, this is the field we will be
 *                        // traversing
 *              {outputVar2 || innerVar2} // the folding expression
 *              {outputVar2} // final (early out) expression
 *          in
 *              project [innerVar2 =                        // compare the field 'b' to 2 and store
 *                         fillEmpty(fieldVar2==2, false)]  // the bool result in innerVar2
 *              limit 1
 *              coscan
 *          from
 *              project [fieldVar2 = getField(fieldVar1, 'b')] // project field 'b' from the
 *                                                             // document  bound to 'fieldVar1',
 *                                                             // which is field 'a'
 *              limit 1
 *              coscan
 *      from
 *         project [fieldVar1 = getField(inputVar, 'a')] // project field 'a' from the document
 *                                                       // bound to 'inputVar'
 *         <inputStage>  // e.g., COLLSCAN
 */
std::pair<sbe::value::SlotId, std::unique_ptr<sbe::PlanStage>> generateTraverseHelper(
    std::unique_ptr<sbe::PlanStage> inputStage,
    sbe::value::SlotId inputVar,
    const FieldPath& fp,
    size_t level,
    sbe::value::SlotIdGenerator* slotIdGenerator,
    const MakePredicateEExprFn& makePredicate,
    LeafArrayTraversalMode mode) {
    using namespace std::literals;

    invariant(level < fp.getPathLength());

    // Generate the projection stage to read a sub-field at the current nested level and bind it
    // to 'fieldVar'.
    std::string_view fieldName{fp.getFieldName(level).rawData(), fp.getFieldName(level).size()};
    auto fieldVar{slotIdGenerator->generate()};
    auto fromBranch = sbe::makeProjectStage(
        std::move(inputStage),
        fieldVar,
        sbe::makeE<sbe::EFunction>("getField"sv,
                                   sbe::makeEs(sbe::makeE<sbe::EVariable>(inputVar),
                                               sbe::makeE<sbe::EConstant>(fieldName))));

    // Generate the 'in' branch for the TraverseStage that we're about to construct.
    auto [innerVar, innerBranch] = (level == fp.getPathLength() - 1u)
        // Base case: Genereate a ProjectStage to evaluate the predicate.
        ? [&]() {
              auto innerVar{slotIdGenerator->generate()};
              return std::make_pair(
                  innerVar,
                  sbe::makeProjectStage(makeLimitCoScanTree(), innerVar, makePredicate(fieldVar)));
          }()
        // Recursive case.
        : generateTraverseHelper(
              makeLimitCoScanTree(), fieldVar, fp, level + 1, slotIdGenerator, makePredicate, mode);

    // Generate the traverse stage for the current nested level.
    auto outputVar{slotIdGenerator->generate()};
    auto outputStage = sbe::makeS<sbe::TraverseStage>(
        std::move(fromBranch),
        std::move(innerBranch),
        fieldVar,
        outputVar,
        innerVar,
        sbe::makeSV(),
        sbe::makeE<sbe::EPrimBinary>(sbe::EPrimBinary::logicOr,
                                     sbe::makeE<sbe::EVariable>(outputVar),
                                     sbe::makeE<sbe::EVariable>(innerVar)),
        sbe::makeE<sbe::EVariable>(outputVar),
        1);

    // For the last level, if `mode` == kArrayAndItsElements and getField() returns an array we
    // need to apply the predicate both to the elements of the array _and_ to the array itself.
    // By itself, TraverseStage only applies the predicate to the elements of the array. Thus,
    // for the last level, we add a ProjectStage so that we also apply the predicate to the array
    // itself. (For cases where getField() doesn't return an array, this additional ProjectStage
    // is effectively a no-op.)
    if (mode == LeafArrayTraversalMode::kArrayAndItsElements && level == fp.getPathLength() - 1u) {
        auto traverseVar = outputVar;
        auto traverseStage = std::move(outputStage);
        outputVar = slotIdGenerator->generate();
        outputStage = sbe::makeProjectStage(
            std::move(traverseStage),
            outputVar,
            sbe::makeE<sbe::EPrimBinary>(
                sbe::EPrimBinary::logicOr,
                makeFillEmptyFalse(sbe::makeE<sbe::EVariable>(traverseVar)),
                sbe::makeE<sbe::EPrimBinary>(
                    sbe::EPrimBinary::logicAnd,
                    makeFillEmptyFalse(sbe::makeE<sbe::EFunction>(
                        "isArray", sbe::makeEs(sbe::makeE<sbe::EVariable>(fieldVar)))),
                    makePredicate(fieldVar))));
    }

    return {outputVar, std::move(outputStage)};
}

/*
 * A helper function for 'generateTraverseForArraySize' similar to the 'generateTraverseHelper'. The
 * function extends the traverse sub-tree generation by retuning a special leaf-level traverse stage
 * that uses a fold expression to add counts of elements in the array, as well as performs an extra
 * check that the leaf-level traversal is being done on a valid array.
 */
std::unique_ptr<sbe::PlanStage> generateTraverseForArraySizeHelper(
    MatchExpressionVisitorContext* context,
    std::unique_ptr<sbe::PlanStage> inputStage,
    sbe::value::SlotId inputVar,
    const SizeMatchExpression* expr,
    size_t level) {
    using namespace std::literals;

    FieldPath path{expr->path()};
    invariant(level < path.getPathLength());

    // The global traversal result.
    const auto& traversePredicateVar = context->predicateVars.top();
    // The field we will be traversing at the current nested level.
    auto fieldVar{context->slotIdGenerator->generate()};
    // The result coming from the 'in' branch of the traverse plan stage.
    auto elemPredicateVar{context->slotIdGenerator->generate()};

    // Generate the projection stage to read a sub-field at the current nested level and bind it
    // to 'fieldVar'.
    inputStage = sbe::makeProjectStage(
        std::move(inputStage),
        fieldVar,
        sbe::makeE<sbe::EFunction>(
            "getField"sv,
            sbe::makeEs(sbe::makeE<sbe::EVariable>(inputVar), sbe::makeE<sbe::EConstant>([&]() {
                            auto fieldName = path.getFieldName(level);
                            return std::string_view{fieldName.rawData(), fieldName.size()};
                        }()))));

    std::unique_ptr<sbe::PlanStage> innerBranch;
    if (level == path.getPathLength() - 1u) {
        // Before generating a final leaf traverse stage, check that the thing we are about to
        // traverse is indeed an array.
        inputStage = sbe::makeS<sbe::FilterStage<false>>(
            std::move(inputStage),
            sbe::makeE<sbe::EFunction>("isArray",
                                       sbe::makeEs(sbe::makeE<sbe::EVariable>(fieldVar))));

        // Project '1' for each element in the array, then sum up using a fold expression.
        innerBranch = sbe::makeProjectStage(
            sbe::makeS<sbe::LimitSkipStage>(sbe::makeS<sbe::CoScanStage>(), 1, boost::none),
            elemPredicateVar,
            sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64, 1));

        // The final traverse stage for the leaf level with a fold expression that sums up
        // values in slot fieldVar, resulting in the count of elements in the array.
        auto leafLevelTraverseStage = sbe::makeS<sbe::TraverseStage>(
            std::move(inputStage),
            std::move(innerBranch),
            fieldVar,
            traversePredicateVar,
            elemPredicateVar,
            sbe::makeSV(),
            sbe::makeE<sbe::EPrimBinary>(sbe::EPrimBinary::add,
                                         sbe::makeE<sbe::EVariable>(traversePredicateVar),
                                         sbe::makeE<sbe::EVariable>(elemPredicateVar)),
            nullptr,
            1);

        auto finalArrayTraverseVar{context->slotIdGenerator->generate()};
        // Final project stage to filter based on the user provided value. If the traversal result
        // was not evaluated to Nothing, then compare to the user provided value. If the traversal
        // final result did evaluate to Nothing, the only way the fold expression result would be
        // Nothing is if the array was empty, so replace Nothing with 0 and compare to the user
        // provided value.
        auto finalProjectStage = sbe::makeProjectStage(
            std::move(leafLevelTraverseStage),
            finalArrayTraverseVar,
            sbe::makeE<sbe::EPrimBinary>(
                sbe::EPrimBinary::eq,
                sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64, expr->getData()),
                sbe::makeE<sbe::EIf>(
                    sbe::makeE<sbe::EFunction>(
                        "exists", sbe::makeEs(sbe::makeE<sbe::EVariable>(traversePredicateVar))),
                    sbe::makeE<sbe::EVariable>(traversePredicateVar),
                    sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64, 0))));

        context->predicateVars.pop();
        context->predicateVars.push(finalArrayTraverseVar);

        return finalProjectStage;
    } else {
        // Generate nested traversal.
        innerBranch = sbe::makeProjectStage(
            generateTraverseForArraySizeHelper(
                context,
                sbe::makeS<sbe::LimitSkipStage>(sbe::makeS<sbe::CoScanStage>(), 1, boost::none),
                fieldVar,
                expr,
                level + 1),
            elemPredicateVar,
            sbe::makeE<sbe::EVariable>(traversePredicateVar));
    }

    // The final traverse stage for the current nested level.
    return sbe::makeS<sbe::TraverseStage>(
        std::move(inputStage),
        std::move(innerBranch),
        fieldVar,
        traversePredicateVar,
        elemPredicateVar,
        sbe::makeSV(),
        sbe::makeE<sbe::EPrimBinary>(sbe::EPrimBinary::logicOr,
                                     sbe::makeE<sbe::EVariable>(traversePredicateVar),
                                     sbe::makeE<sbe::EVariable>(elemPredicateVar)),
        sbe::makeE<sbe::EVariable>(traversePredicateVar),
        1);
}

/**
 * For the given PathMatchExpression 'expr', generates a path traversal SBE plan stage sub-tree
 * implementing the expression. Generates a sequence of nested traverse operators in order to
 * perform nested array traversal, and then calls 'makeEExprCallback' in order to generate an SBE
 * expression responsible for applying the predicate to individual array elements.
 */
void generateTraverse(MatchExpressionVisitorContext* context,
                      const PathMatchExpression* expr,
                      MakePredicateEExprFn makePredicate) {
    FieldPath fp{expr->path()};

    auto [slot, stage] = generateTraverseHelper(std::move(context->inputStage),
                                                context->inputVar,
                                                fp,
                                                0,
                                                context->slotIdGenerator,
                                                makePredicate,
                                                LeafArrayTraversalMode::kArrayAndItsElements);

    context->predicateVars.push(slot);
    context->inputStage = std::move(stage);

    // Check if can bail out early from the $and predicate if this expression is part of branch.
    checkForShortCircuitFromLogicalAnd(context);
}

/**
 * Generates a path traversal SBE plan stage sub-tree for matching arrays with '$size'. Applies
 * an extra project on top of the sub-tree to filter based on user provided value.
 */
void generateTraverseForArraySize(MatchExpressionVisitorContext* context,
                                  const SizeMatchExpression* expr) {
    context->predicateVars.push(context->slotIdGenerator->generate());
    context->inputStage = generateTraverseForArraySizeHelper(
        context, std::move(context->inputStage), context->inputVar, expr, 0);

    // Check if can bail out early from the $and predicate if this expression is part of branch.
    checkForShortCircuitFromLogicalAnd(context);
}

/**
 * Generates a path traversal SBE plan stage sub-tree which implments the comparison match
 * expression 'expr'. The comparison itself executes using the given 'binaryOp'.
 */
void generateTraverseForComparisonPredicate(MatchExpressionVisitorContext* context,
                                            const ComparisonMatchExpression* expr,
                                            sbe::EPrimBinary::Op binaryOp) {
    auto makeEExprFn = [expr, binaryOp](sbe::value::SlotId inputSlot) {
        const auto& rhs = expr->getData();
        auto [tagView, valView] = sbe::bson::convertFrom(
            true, rhs.rawdata(), rhs.rawdata() + rhs.size(), rhs.fieldNameSize() - 1);

        // SBE EConstant assumes ownership of the value so we have to make a copy here.
        auto [tag, val] = sbe::value::copyValue(tagView, valView);

        return makeFillEmptyFalse(sbe::makeE<sbe::EPrimBinary>(
            binaryOp, sbe::makeE<sbe::EVariable>(inputSlot), sbe::makeE<sbe::EConstant>(tag, val)));
    };
    generateTraverse(context, expr, std::move(makeEExprFn));
}

/**
 * Generates an SBE plan stage sub-tree implementing a logical $or expression.
 */
void generateLogicalOr(MatchExpressionVisitorContext* context, const OrMatchExpression* expr) {
    invariant(!context->predicateVars.empty());
    invariant(context->predicateVars.size() >= expr->numChildren());

    auto filter = sbe::makeE<sbe::EVariable>(context->predicateVars.top());
    context->predicateVars.pop();

    auto numOrBranches = expr->numChildren() - 1;
    for (size_t childNum = 0; childNum < numOrBranches; ++childNum) {
        filter =
            sbe::makeE<sbe::EPrimBinary>(sbe::EPrimBinary::logicOr,
                                         std::move(filter),
                                         sbe::makeE<sbe::EVariable>(context->predicateVars.top()));
        context->predicateVars.pop();
    }

    // If this $or expression is a branch of another $and expression, or is a top-level logical
    // expression we can just inject a filter stage without propagating the result of the predicate
    // evaluation to the parent expression, to form a sub-tree of stage->FILTER->stage->FILTER plan
    // stages to support early exit for the $and branches. Otherwise, just project out the result
    // of the predicate evaluation and let the parent expression handle it.
    if (context->nestedLogicalExprs.empty() ||
        context->nestedLogicalExprs.top().first->matchType() == MatchExpression::AND) {
        context->inputStage =
            sbe::makeS<sbe::FilterStage<false>>(std::move(context->inputStage), std::move(filter));
    } else {
        context->predicateVars.push(context->slotIdGenerator->generate());
        context->inputStage = sbe::makeProjectStage(
            std::move(context->inputStage), context->predicateVars.top(), std::move(filter));
    }
}

/**
 * Generates an SBE plan stage sub-tree implementing a logical $and expression.
 */
void generateLogicalAnd(MatchExpressionVisitorContext* context, const AndMatchExpression* expr) {
    auto filter = [&]() {
        if (expr->numChildren() > 0) {
            invariant(!context->predicateVars.empty());
            auto predicateVar = context->predicateVars.top();
            context->predicateVars.pop();
            return sbe::makeE<sbe::EVariable>(predicateVar);
        } else {
            return sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Boolean, 1);
        }
    }();

    // If this $and expression is a branch of another $and expression, or is a top-level logical
    // expression we can just inject a filter stage without propagating the result of the predicate
    // evaluation to the parent expression, to form a sub-tree of stage->FILTER->stage->FILTER plan
    // stages to support early exit for the $and branches. Otherwise, just project out the result
    // of the predicate evaluation and let the parent expression handle it.
    if (context->nestedLogicalExprs.empty() ||
        context->nestedLogicalExprs.top().first->matchType() == MatchExpression::AND) {
        context->inputStage =
            sbe::makeS<sbe::FilterStage<false>>(std::move(context->inputStage), std::move(filter));
    } else {
        context->predicateVars.push(context->slotIdGenerator->generate());
        context->inputStage = sbe::makeProjectStage(
            std::move(context->inputStage), context->predicateVars.top(), std::move(filter));
    }
}

/**
 * Generates and pushes a constant boolean expression for either alwaysTrue or alwaysFalse.
 */
void generateAlwaysBoolean(MatchExpressionVisitorContext* context, bool value) {
    context->predicateVars.push(context->slotIdGenerator->generate());
    context->inputStage =
        sbe::makeProjectStage(std::move(context->inputStage),
                              context->predicateVars.top(),
                              sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Boolean, value));

    // Check if can bail out early from the $and predicate if this expression is part of branch.
    checkForShortCircuitFromLogicalAnd(context);
}

/**
 * Generates a SBE plan stage sub-tree which implements the bitwise match expression 'expr'. The
 * various bit test expressions accept a numeric, BinData or position list bitmask. Here we handle
 * building an EExpression for both the numeric and BinData or position list forms of the bitmask.
 */
void generateTraverseForBitTests(MatchExpressionVisitorContext* context,
                                 const BitTestMatchExpression* expr,
                                 const sbe::BitTestBehavior& bitTestBehavior) {
    auto makeEExprFn = [expr, bitTestBehavior](sbe::value::SlotId inputSlot) {
        auto bitPositions = expr->getBitPositions();

        // Build an array set of bit positions for the bitmask, and remove duplicates in the
        // bitPositions vector since duplicates aren't handled in the match expression parser by
        // checking if an item has already been seen.
        auto [bitPosTag, bitPosVal] = sbe::value::makeNewArray();
        auto arr = sbe::value::getArrayView(bitPosVal);
        arr->reserve(bitPositions.size());

        std::set<int> seenBits;
        for (size_t index = 0; index < bitPositions.size(); ++index) {
            auto currentBit = bitPositions[index];
            if (auto result = seenBits.insert(currentBit); result.second) {
                arr->push_back(sbe::value::TypeTags::NumberInt64, currentBit);
            }
        }

        // An EExpression for the BinData and position list for the binary case of
        // BitTestMatchExpressions. This function will be applied to values carrying BinData
        // elements.
        auto binaryBitTestEExpr = sbe::makeE<sbe::EFunction>(
            "bitTestPosition",
            sbe::makeEs(sbe::makeE<sbe::EConstant>(bitPosTag, bitPosVal),
                        sbe::makeE<sbe::EVariable>(inputSlot),
                        sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32,
                                                   static_cast<int32_t>(bitTestBehavior))));

        // Build An EExpression for the numeric bitmask case. The AllSet case tests if (mask &
        // value) == mask, and AllClear case tests if (mask & value) == 0. The AnyClear and the
        // AnySet case is the negation of the AllSet and AllClear cases, respectively.
        auto numericBitTestEExpr =
            sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64, expr->getBitMask());
        if (bitTestBehavior == sbe::BitTestBehavior::AllSet ||
            bitTestBehavior == sbe::BitTestBehavior::AnyClear) {
            numericBitTestEExpr = sbe::makeE<sbe::EFunction>(
                "bitTestMask",
                sbe::makeEs(std::move(numericBitTestEExpr), sbe::makeE<sbe::EVariable>(inputSlot)));

            // The AnyClear case is the negation of the AllSet case.
            if (bitTestBehavior == sbe::BitTestBehavior::AnyClear) {
                numericBitTestEExpr = sbe::makeE<sbe::EPrimUnary>(sbe::EPrimUnary::logicNot,
                                                                  std::move(numericBitTestEExpr));
            }
        } else if (bitTestBehavior == sbe::BitTestBehavior::AllClear ||
                   bitTestBehavior == sbe::BitTestBehavior::AnySet) {
            numericBitTestEExpr = sbe::makeE<sbe::EFunction>(
                "bitTestZero",
                sbe::makeEs(std::move(numericBitTestEExpr), sbe::makeE<sbe::EVariable>(inputSlot)));

            // The AnySet case is the negation of the AllClear case.
            if (bitTestBehavior == sbe::BitTestBehavior::AnySet) {
                numericBitTestEExpr = sbe::makeE<sbe::EPrimUnary>(sbe::EPrimUnary::logicNot,
                                                                  std::move(numericBitTestEExpr));
            }
        } else {
            MONGO_UNREACHABLE;
        }
        return sbe::makeE<sbe::EIf>(
            sbe::makeE<sbe::EFunction>("isBinData",
                                       sbe::makeEs(sbe::makeE<sbe::EVariable>(inputSlot))),
            std::move(binaryBitTestEExpr),
            std::move(numericBitTestEExpr));
    };
    generateTraverse(context, expr, std::move(makeEExprFn));
}

/**
 * A match expression pre-visitor used for maintaining nested logical expressions while traversing
 * the match expression tree.
 */
class MatchExpressionPreVisitor final : public MatchExpressionConstVisitor {
public:
    MatchExpressionPreVisitor(MatchExpressionVisitorContext* context) : _context(context) {}

    void visit(const AlwaysFalseMatchExpression* expr) final {}
    void visit(const AlwaysTrueMatchExpression* expr) final {}
    void visit(const AndMatchExpression* expr) final {
        _context->nestedLogicalExprs.push({expr, expr->numChildren()});
    }
    void visit(const BitsAllClearMatchExpression* expr) final {}
    void visit(const BitsAllSetMatchExpression* expr) final {}
    void visit(const BitsAnyClearMatchExpression* expr) final {}
    void visit(const BitsAnySetMatchExpression* expr) final {}
    void visit(const ElemMatchObjectMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const ElemMatchValueMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const EqualityMatchExpression* expr) final {}
    void visit(const ExistsMatchExpression* expr) final {}
    void visit(const ExprMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const GTEMatchExpression* expr) final {}
    void visit(const GTMatchExpression* expr) final {}
    void visit(const GeoMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const GeoNearMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InternalExprEqMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InternalSchemaAllElemMatchFromIndexMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InternalSchemaAllowedPropertiesMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InternalSchemaBinDataEncryptedTypeExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InternalSchemaBinDataSubTypeExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InternalSchemaCondMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InternalSchemaEqMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InternalSchemaFmodMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InternalSchemaMatchArrayIndexMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InternalSchemaMaxItemsMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InternalSchemaMaxLengthMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InternalSchemaMaxPropertiesMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InternalSchemaMinItemsMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InternalSchemaMinLengthMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InternalSchemaMinPropertiesMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InternalSchemaObjectMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InternalSchemaRootDocEqMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InternalSchemaTypeExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InternalSchemaUniqueItemsMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InternalSchemaXorMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const LTEMatchExpression* expr) final {}
    void visit(const LTMatchExpression* expr) final {}
    void visit(const ModMatchExpression* expr) final {}
    void visit(const NorMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const NotMatchExpression* expr) final {
        invariant(expr->numChildren() == 1);
        _context->nestedLogicalExprs.push({expr, expr->numChildren()});
    }
    void visit(const OrMatchExpression* expr) final {
        _context->nestedLogicalExprs.push({expr, expr->numChildren()});
    }
    void visit(const RegexMatchExpression* expr) final {}
    void visit(const SizeMatchExpression* expr) final {}
    void visit(const TextMatchExpression* expr) final {
        // The QueryPlanner always converts a $text predicate into a query solution involving the
        // 'TextNode' which is translated to an SBE plan elsewhere. Therefore, no $text predicates
        // should remain in the MatchExpression tree when converting it to SBE.
        MONGO_UNREACHABLE;
    }
    void visit(const TextNoOpMatchExpression* expr) final {
        // No-op $text match expressions exist as a crutch for parsing a $text predicate without
        // having access to the FTS subsystem. We should never attempt to execute a MatchExpression
        // containing such a no-op node.
        MONGO_UNREACHABLE;
    }
    void visit(const TwoDPtInAnnulusExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const TypeMatchExpression* expr) final {}
    void visit(const WhereMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const WhereNoOpMatchExpression* expr) final {
        unsupportedExpression(expr);
    }

private:
    void unsupportedExpression(const MatchExpression* expr) const {
        uasserted(4822878,
                  str::stream() << "Match expression is not supported in SBE: "
                                << expr->matchType());
    }

    MatchExpressionVisitorContext* _context;
};

/**
 * A match expression post-visitor which does all the job to translate the match expression tree
 * into an SBE plan stage sub-tree.
 */
class MatchExpressionPostVisitor final : public MatchExpressionConstVisitor {
public:
    MatchExpressionPostVisitor(MatchExpressionVisitorContext* context) : _context(context) {}

    void visit(const AlwaysFalseMatchExpression* expr) final {
        generateAlwaysBoolean(_context, false);
    }

    void visit(const AlwaysTrueMatchExpression* expr) final {
        generateAlwaysBoolean(_context, true);
    }

    void visit(const AndMatchExpression* expr) final {
        invariant(!_context->nestedLogicalExprs.empty());
        _context->nestedLogicalExprs.pop();
        generateLogicalAnd(_context, expr);
    }

    void visit(const BitsAllClearMatchExpression* expr) final {
        generateTraverseForBitTests(_context, expr, sbe::BitTestBehavior::AllClear);
    }

    void visit(const BitsAllSetMatchExpression* expr) final {
        generateTraverseForBitTests(_context, expr, sbe::BitTestBehavior::AllSet);
    }

    void visit(const BitsAnyClearMatchExpression* expr) final {
        generateTraverseForBitTests(_context, expr, sbe::BitTestBehavior::AnyClear);
    }

    void visit(const BitsAnySetMatchExpression* expr) final {
        generateTraverseForBitTests(_context, expr, sbe::BitTestBehavior::AnySet);
    }

    void visit(const ElemMatchObjectMatchExpression* expr) final {}
    void visit(const ElemMatchValueMatchExpression* expr) final {}

    void visit(const EqualityMatchExpression* expr) final {
        generateTraverseForComparisonPredicate(_context, expr, sbe::EPrimBinary::eq);
    }

    void visit(const ExistsMatchExpression* expr) final {
        auto makeEExprFn = [](sbe::value::SlotId inputSlot) {
            return sbe::makeE<sbe::EFunction>("exists",
                                              sbe::makeEs(sbe::makeE<sbe::EVariable>(inputSlot)));
        };
        generateTraverse(_context, expr, std::move(makeEExprFn));
    }

    void visit(const ExprMatchExpression* expr) final {}

    void visit(const GTEMatchExpression* expr) final {
        generateTraverseForComparisonPredicate(_context, expr, sbe::EPrimBinary::greaterEq);
    }

    void visit(const GTMatchExpression* expr) final {
        generateTraverseForComparisonPredicate(_context, expr, sbe::EPrimBinary::greater);
    }

    void visit(const GeoMatchExpression* expr) final {}
    void visit(const GeoNearMatchExpression* expr) final {}
    void visit(const InMatchExpression* expr) final {}
    void visit(const InternalExprEqMatchExpression* expr) final {}
    void visit(const InternalSchemaAllElemMatchFromIndexMatchExpression* expr) final {}
    void visit(const InternalSchemaAllowedPropertiesMatchExpression* expr) final {}
    void visit(const InternalSchemaBinDataEncryptedTypeExpression* expr) final {}
    void visit(const InternalSchemaBinDataSubTypeExpression* expr) final {}
    void visit(const InternalSchemaCondMatchExpression* expr) final {}
    void visit(const InternalSchemaEqMatchExpression* expr) final {}
    void visit(const InternalSchemaFmodMatchExpression* expr) final {}
    void visit(const InternalSchemaMatchArrayIndexMatchExpression* expr) final {}
    void visit(const InternalSchemaMaxItemsMatchExpression* expr) final {}
    void visit(const InternalSchemaMaxLengthMatchExpression* expr) final {}
    void visit(const InternalSchemaMaxPropertiesMatchExpression* expr) final {}
    void visit(const InternalSchemaMinItemsMatchExpression* expr) final {}
    void visit(const InternalSchemaMinLengthMatchExpression* expr) final {}
    void visit(const InternalSchemaMinPropertiesMatchExpression* expr) final {}
    void visit(const InternalSchemaObjectMatchExpression* expr) final {}
    void visit(const InternalSchemaRootDocEqMatchExpression* expr) final {}
    void visit(const InternalSchemaTypeExpression* expr) final {}
    void visit(const InternalSchemaUniqueItemsMatchExpression* expr) final {}
    void visit(const InternalSchemaXorMatchExpression* expr) final {}

    void visit(const LTEMatchExpression* expr) final {
        generateTraverseForComparisonPredicate(_context, expr, sbe::EPrimBinary::lessEq);
    }

    void visit(const LTMatchExpression* expr) final {
        generateTraverseForComparisonPredicate(_context, expr, sbe::EPrimBinary::less);
    }

    void visit(const ModMatchExpression* expr) final {
        // The mod function returns the result of the mod operation between the operand and given
        // divisor, so construct an expression to then compare the result of the operation to the
        // given remainder.
        auto makeEExprFn = [expr](sbe::value::SlotId inputSlot) {
            return makeFillEmptyFalse(sbe::makeE<sbe::EPrimBinary>(
                sbe::EPrimBinary::eq,
                sbe::makeE<sbe::EFunction>(
                    "mod",
                    sbe::makeEs(sbe::makeE<sbe::EVariable>(inputSlot),
                                sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64,
                                                           expr->getDivisor()))),
                sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64,
                                           expr->getRemainder())));
        };

        generateTraverse(_context, expr, std::move(makeEExprFn));
    }

    void visit(const NorMatchExpression* expr) final {}

    void visit(const NotMatchExpression* expr) final {
        invariant(!_context->nestedLogicalExprs.empty());
        invariant(!_context->predicateVars.empty());
        _context->nestedLogicalExprs.pop();

        auto filter = sbe::makeE<sbe::EPrimUnary>(
            sbe::EPrimUnary::logicNot,
            generateExpressionForLogicBranch(sbe::EVariable{_context->predicateVars.top()}));
        _context->predicateVars.pop();

        _context->predicateVars.push(_context->slotIdGenerator->generate());
        _context->inputStage = sbe::makeProjectStage(
            std::move(_context->inputStage), _context->predicateVars.top(), std::move(filter));

        checkForShortCircuitFromLogicalAnd(_context);
    }

    void visit(const OrMatchExpression* expr) final {
        invariant(!_context->nestedLogicalExprs.empty());
        _context->nestedLogicalExprs.pop();
        generateLogicalOr(_context, expr);
    }

    void visit(const RegexMatchExpression* expr) final {
        auto makeEExprFn = [expr](sbe::value::SlotId inputSlot) {
            auto regex = RegexMatchExpression::makeRegex(expr->getString(), expr->getFlags());
            auto ownedRegexVal = sbe::value::bitcastFrom(regex.release());

            // TODO: In the future, this needs to account for the fact that the regex match
            // expression matches strings, but also matches stored regexes. For example,
            // {$match: {a: /foo/}} matches the document {a: /foo/} in addition to {a: "foobar"}.
            return makeFillEmptyFalse(sbe::makeE<sbe::EFunction>(
                "regexMatch",
                sbe::makeEs(
                    sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::pcreRegex, ownedRegexVal),
                    sbe::makeE<sbe::EVariable>(inputSlot))));
        };

        generateTraverse(_context, expr, std::move(makeEExprFn));
    }

    void visit(const SizeMatchExpression* expr) final {
        generateTraverseForArraySize(_context, expr);
    }

    void visit(const TextMatchExpression* expr) final {}
    void visit(const TextNoOpMatchExpression* expr) final {}
    void visit(const TwoDPtInAnnulusExpression* expr) final {}

    void visit(const TypeMatchExpression* expr) final {
        auto makeEExprFn = [expr](sbe::value::SlotId inputSlot) {
            const MatcherTypeSet& ts = expr->typeSet();
            return sbe::makeE<sbe::ETypeMatch>(sbe::makeE<sbe::EVariable>(inputSlot),
                                               ts.getBSONTypeMask());
        };
        generateTraverse(_context, expr, std::move(makeEExprFn));
    }

    void visit(const WhereMatchExpression* expr) final {}
    void visit(const WhereNoOpMatchExpression* expr) final {}

private:
    MatchExpressionVisitorContext* _context;
};

/**
 * A match expression in-visitor used for maintaining the counter of the processed child expressions
 * of the nested logical expressions in the match expression tree being traversed.
 */
class MatchExpressionInVisitor final : public MatchExpressionConstVisitor {
public:
    MatchExpressionInVisitor(MatchExpressionVisitorContext* context) : _context(context) {}

    void visit(const AlwaysFalseMatchExpression* expr) final {}
    void visit(const AlwaysTrueMatchExpression* expr) final {}

    void visit(const AndMatchExpression* expr) final {
        invariant(_context->nestedLogicalExprs.top().first == expr);
        _context->nestedLogicalExprs.top().second--;
    }

    void visit(const BitsAllClearMatchExpression* expr) final {}
    void visit(const BitsAllSetMatchExpression* expr) final {}
    void visit(const BitsAnyClearMatchExpression* expr) final {}
    void visit(const BitsAnySetMatchExpression* expr) final {}
    void visit(const ElemMatchObjectMatchExpression* expr) final {}
    void visit(const ElemMatchValueMatchExpression* expr) final {}
    void visit(const EqualityMatchExpression* expr) final {}
    void visit(const ExistsMatchExpression* expr) final {}
    void visit(const ExprMatchExpression* expr) final {}
    void visit(const GTEMatchExpression* expr) final {}
    void visit(const GTMatchExpression* expr) final {}
    void visit(const GeoMatchExpression* expr) final {}
    void visit(const GeoNearMatchExpression* expr) final {}
    void visit(const InMatchExpression* expr) final {}
    void visit(const InternalExprEqMatchExpression* expr) final {}
    void visit(const InternalSchemaAllElemMatchFromIndexMatchExpression* expr) final {}
    void visit(const InternalSchemaAllowedPropertiesMatchExpression* expr) final {}
    void visit(const InternalSchemaBinDataEncryptedTypeExpression* expr) final {}
    void visit(const InternalSchemaBinDataSubTypeExpression* expr) final {}
    void visit(const InternalSchemaCondMatchExpression* expr) final {}
    void visit(const InternalSchemaEqMatchExpression* expr) final {}
    void visit(const InternalSchemaFmodMatchExpression* expr) final {}
    void visit(const InternalSchemaMatchArrayIndexMatchExpression* expr) final {}
    void visit(const InternalSchemaMaxItemsMatchExpression* expr) final {}
    void visit(const InternalSchemaMaxLengthMatchExpression* expr) final {}
    void visit(const InternalSchemaMaxPropertiesMatchExpression* expr) final {}
    void visit(const InternalSchemaMinItemsMatchExpression* expr) final {}
    void visit(const InternalSchemaMinLengthMatchExpression* expr) final {}
    void visit(const InternalSchemaMinPropertiesMatchExpression* expr) final {}
    void visit(const InternalSchemaObjectMatchExpression* expr) final {}
    void visit(const InternalSchemaRootDocEqMatchExpression* expr) final {}
    void visit(const InternalSchemaTypeExpression* expr) final {}
    void visit(const InternalSchemaUniqueItemsMatchExpression* expr) final {}
    void visit(const InternalSchemaXorMatchExpression* expr) final {}
    void visit(const LTEMatchExpression* expr) final {}
    void visit(const LTMatchExpression* expr) final {}
    void visit(const ModMatchExpression* expr) final {}
    void visit(const NorMatchExpression* expr) final {}
    void visit(const NotMatchExpression* expr) final {}

    void visit(const OrMatchExpression* expr) final {
        invariant(_context->nestedLogicalExprs.top().first == expr);
        _context->nestedLogicalExprs.top().second--;
    }

    void visit(const RegexMatchExpression* expr) final {}
    void visit(const SizeMatchExpression* expr) final {}
    void visit(const TextMatchExpression* expr) final {}
    void visit(const TextNoOpMatchExpression* expr) final {}
    void visit(const TwoDPtInAnnulusExpression* expr) final {}
    void visit(const TypeMatchExpression* expr) final {}
    void visit(const WhereMatchExpression* expr) final {}
    void visit(const WhereNoOpMatchExpression* expr) final {}

private:
    MatchExpressionVisitorContext* _context;
};
}  // namespace

std::unique_ptr<sbe::PlanStage> generateFilter(const MatchExpression* root,
                                               std::unique_ptr<sbe::PlanStage> stage,
                                               sbe::value::SlotIdGenerator* slotIdGenerator,
                                               sbe::value::SlotId inputVar) {
    // The planner adds an $and expression without the operands if the query was empty. We can bail
    // out early without generating the filter plan stage if this is the case.
    if (root->matchType() == MatchExpression::AND && root->numChildren() == 0) {
        return stage;
    }

    MatchExpressionVisitorContext context{slotIdGenerator, std::move(stage), inputVar};
    MatchExpressionPreVisitor preVisitor{&context};
    MatchExpressionInVisitor inVisitor{&context};
    MatchExpressionPostVisitor postVisitor{&context};
    MatchExpressionWalker walker{&preVisitor, &inVisitor, &postVisitor};
    tree_walker::walk<true, MatchExpression>(root, &walker);
    return context.done();
}
}  // namespace mongo::stage_builder
