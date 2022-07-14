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

#include <functional>

#include "mongo/db/exec/sbe/stages/co_scan.h"
#include "mongo/db/exec/sbe/stages/filter.h"
#include "mongo/db/exec/sbe/stages/limit_skip.h"
#include "mongo/db/exec/sbe/stages/loop_join.h"
#include "mongo/db/exec/sbe/stages/project.h"
#include "mongo/db/exec/sbe/stages/traverse.h"
#include "mongo/db/exec/sbe/stages/union.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_expr.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/matcher/expression_internal_expr_comparison.h"
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
#include "mongo/db/query/sbe_stage_builder.h"
#include "mongo/db/query/sbe_stage_builder_eval_frame.h"
#include "mongo/db/query/sbe_stage_builder_expression.h"
#include "mongo/db/query/util/make_data_structure.h"
#include "mongo/util/str.h"

namespace mongo::stage_builder {
namespace {

struct MatchExpressionVisitorContext;
const size_t kMaxChildrenForTopLevelAndOptimization = 25;

/**
 * Output of the tree can come from two places:
 *  - If there is an expression on the evaluation stack in the end of tree construction, then this
 *    is the output for the whole tree. This is checked in the 'MatchExpressionVisitorContext::done'
 *    method.
 *  - If we apply top-level AND optimization, then in the end of tree construction the evaluation
 *    stack will be empty. This happens because expressions which normally would reside on the stack
 *    are popped and inserted directly into the filter stage for each branch.
 *
 * So, we need to record output in both the 'MatchExpressionVisitorContext::done' method and builder
 * for top-level AND.
 *
 * This function takes the current expression, projects it into a separate slot and stores this slot
 * as an output for the current frame.
 */
void projectCurrentExprToOutputSlot(MatchExpressionVisitorContext* context);

/**
 * The various flavors of PathMatchExpressions require the same skeleton of traverse operators in
 * order to perform implicit path traversal, but may translate differently to an SBE expression that
 * actually applies the predicate against an individual array element.
 *
 * A function of this type can be called to generate an EExpression which applies a predicate to the
 * value found in 'inputSlot'.
 */
using MakePredicateFn =
    std::function<EvalExprStagePair(sbe::value::SlotId inputSlot, EvalStage inputStage)>;

/**
 * A struct for storing context across calls to visit() methods in MatchExpressionVisitor's.
 */
struct MatchExpressionVisitorContext {
    // Construct a visitor context to generate a filter expression from a single input slot
    // holding a document against which to perform the match.
    MatchExpressionVisitorContext(StageBuilderState& state,
                                  EvalStage inputStage,
                                  sbe::value::SlotId inputSlot,
                                  const MatchExpression* root,
                                  PlanNodeId planNodeId,
                                  const FilterStateHelper& stateHelper)
        : state{state},
          inputSlot{inputSlot},
          topLevelAnd{nullptr},
          planNodeId{planNodeId},
          stateHelper{stateHelper} {
        // Set up the top-level EvalFrame.
        evalStack.emplaceFrame(std::move(inputStage), inputSlot);

        // If the root node is an $and, store it in 'topLevelAnd'.
        // TODO: SERVER-50673: Revisit how we implement the top-level $and optimization.
        if (root->matchType() == MatchExpression::AND &&
            root->numChildren() <= kMaxChildrenForTopLevelAndOptimization) {
            topLevelAnd = root;
        }
    }

    // Construct a visitor context to generate a filter expression that is attached to an index
    // scan and can evaluate an expression from the index keys without fetching an entire document.
    // Instead of a single input slot holding the root document, it takes a vector of 'keySlots' and
    // 'keyFields' which represent a subset of the fields of the index key pattern that are depended
    // on to evaluate the predicate, and corresponding slots for each of the key fields.
    MatchExpressionVisitorContext(StageBuilderState& state,
                                  EvalStage inputStage,
                                  sbe::value::SlotVector keySlots,
                                  std::vector<std::string> keyFields,
                                  const MatchExpression* root,
                                  PlanNodeId planNodeId,
                                  const FilterStateHelper& stateHelper)
        : state{state}, topLevelAnd{nullptr}, planNodeId{planNodeId}, stateHelper{stateHelper} {
        // Set up the top-level EvalFrame.
        evalStack.emplaceFrame(std::move(inputStage), boost::none);

        tassert(5273400, "Index key slots vector is empty", keySlots.size() > 0);
        tassert(5273401,
                "Mismatch between index key slots and fields",
                keySlots.size() == keyFields.size());

        for (size_t idx = 0; idx < keySlots.size(); ++idx) {
            auto&& field = keyFields[idx];
            tassert(5273410, "Index key field is empty", !field.empty());
            indexKeySlots[field] = keySlots[idx];
        }

        // If the root node is an $and, store it in 'topLevelAnd'.
        // TODO: SERVER-50673: Revisit how we implement the top-level $and optimization.
        if (root->matchType() == MatchExpression::AND &&
            root->numChildren() <= kMaxChildrenForTopLevelAndOptimization) {
            topLevelAnd = root;
        }
    }

    std::pair<boost::optional<sbe::value::SlotId>, EvalStage> done() {
        invariant(evalStack.framesCount() == 1);
        auto& frame = evalStack.topFrame();

        if (frame.exprsCount() > 0) {
            if (stateHelper.stateContainsValue()) {
                projectCurrentExprToOutputSlot(this);
            }
            invariant(frame.exprsCount() == 1);
            frame.setStage(makeFilter<false>(frame.extractStage(),
                                             stateHelper.getBool(frame.popExpr().extractExpr()),
                                             planNodeId));
        }

        if (outputSlot && stateHelper.stateContainsValue()) {
            // In case 'outputSlot' is defined and state contains a value, we need to extract this
            // value into a separate slot and return it. The resulting value depends on the state
            // type, see the implementation of specific state helper for details.
            return stateHelper.projectValueCombinator(*outputSlot,
                                                      frame.extractStage(),
                                                      planNodeId,
                                                      state.slotIdGenerator,
                                                      state.frameIdGenerator);
        }

        return {boost::none, frame.extractStage()};
    }

    struct FrameData {
        // For an index filter we don't build a traversal sub-tree, and do not use complex
        // expressions, such as $elemMatch or nested logical $and/$or/$nor. As such, we don't need
        // to create nested EvalFrames, and we don't need an 'inputSlot' for the frame, because
        // values are read from the 'indexKeySlots' map  stored in the context. Yet, we still need a
        // top-level EvalFrame, as the the entire filter generator logic is based on the assumption
        // that we've got at least one EvalFrame. Hence, the 'inputSlot' is declared optional.
        boost::optional<sbe::value::SlotId> inputSlot;

        FrameData(boost::optional<sbe::value::SlotId> inputSlot) : inputSlot{inputSlot} {}
    };

    StageBuilderState& state;

    EvalStack<FrameData> evalStack;
    // The current context must be initialized either with an 'inputSlot' over which an entire match
    // expression needs to be evaluated, or a pair of 'keySlots' and 'keyFields' vectors
    // representing a subset of the fields of the index key pattern that are depended on to evaluate
    // the predicate, and corresponding slots for each of the fields, which are stored in
    // 'indexKeySlots' map.
    boost::optional<sbe::value::SlotId> inputSlot;
    StringMap<sbe::value::SlotId> indexKeySlots;
    const MatchExpression* topLevelAnd;

    // The id of the 'QuerySolutionNode' which houses the match expression that we are converting to
    // SBE.
    const PlanNodeId planNodeId;

    // Helper for managing the internal state of the filter tree. See 'FilterStateHelper' definition
    // for details.
    const FilterStateHelper& stateHelper;

    // Trees for some queries can have something to output. For instance, if we use
    // 'IndexStateHelper' for managing internal state, this output is the index of the array element
    // that matched our query predicate. This field stores the slot id containing the output of the
    // tree.
    boost::optional<sbe::value::SlotId> outputSlot;
};

void projectCurrentExprToOutputSlot(MatchExpressionVisitorContext* context) {
    tassert(5291405, "Output slot is not empty", !context->outputSlot);
    auto& frame = context->evalStack.topFrame();
    auto [projectedExprSlot, stage] = projectEvalExpr(
        frame.popExpr(), frame.extractStage(), context->planNodeId, context->state.slotIdGenerator);
    context->outputSlot = projectedExprSlot;
    frame.pushExpr(projectedExprSlot);
    frame.setStage(std::move(stage));
}

enum class LeafTraversalMode {
    // Don't generate a TraverseStage for the leaf.
    kDoNotTraverseLeaf = 0,

    // Traverse the leaf, ard for arrays visit both the array's elements _and_ the array itself.
    kArrayAndItsElements = 1,

    // Traverse the leaf, and for arrays visit the array's elements but not the array itself.
    kArrayElementsOnly = 2,
};

/**
 * This function generates a path traversal plan stage at the given nested 'level' of the traversal
 * path. For example, for a dotted path expression {'a.b': 2}, the traversal sub-tree built with
 * 'BooleanStateHelper' will look like this:
 *
 *     traverse
 *         outputSlot1 // the traversal result
 *         innerSlot1  // the result coming from the 'in' branch
 *         fieldSlot1  // field 'a' projected in the 'from' branch, this is the field we will be
 *                     // traversing
 *         {outputSlot1 || innerSlot1} // the folding expression - combining results for each
 *                                     // element
 *         {outputSlot1} // final (early out) expression - when we hit the 'true' value, we don't
 *                       // have to traverse the whole array
 *     from
 *         project [fieldSlot1 = getField(inputSlot, "a")] // project field 'a' from the document
 *                                                         // bound to 'inputSlot'
 *         <inputStage> // e.g. collection scan
 *     in
 *         project [innerSlot1 =                                   // if getField(fieldSlot1,'b')
 *                      fillEmpty(outputSlot2, false) ||           // returns an array, compare the
 *                      (fillEmpty(isArray(fieldSlot2), false) &&  // array itself to 2 as well
 *                       fillEmpty(fieldSlot2 == 2, false))]
 *         traverse // nested traversal
 *             outputSlot2 // the traversal result
 *             innerSlot2  // the result coming from the 'in' branch
 *             fieldSlot2  // field 'b' projected in the 'from' branch, this is the field we will be
 *                         // traversing
 *             {outputSlot2 || innerSlot2} // the folding expression
 *             {outputSlot2} // final (early out) expression
 *         from
 *             project [fieldSlot2 = getField(fieldSlot1, "b")] // project field 'b' from the
 *                                                               // document  bound to 'fieldSlot1',
 *                                                               // which is field 'a'
 *             limit 1
 *             coscan
 *         in
 *             project [innerSlot2 =                            // compare the field 'b' to 2 and
 *                          fillEmpty(fieldSlot2 == 2, false)] // store the result in innerSlot2
 *             limit 1
 *             coscan
 */
EvalExprStagePair generatePathTraversal(EvalStage inputStage,
                                        sbe::value::SlotId inputDocumentSlot,
                                        const FieldRef& fp,
                                        FieldIndex level,
                                        PlanNodeId planNodeId,
                                        sbe::value::SlotIdGenerator* slotIdGenerator,
                                        sbe::value::FrameIdGenerator* frameIdGenerator,
                                        const MakePredicateFn& makePredicate,
                                        LeafTraversalMode mode,
                                        const FilterStateHelper& stateHelper) {
    using namespace std::literals;

    invariant(level < fp.numParts());

    const bool isLeafField = (level == fp.numParts() - 1u);
    const bool needsArrayCheck = isLeafField && mode == LeafTraversalMode::kArrayAndItsElements;

    // Generate the projection stage to read a sub-field at the current nested level and bind it
    // to 'inputSlot'.
    auto fieldName = fp.getPart(level);
    auto inputSlot = slotIdGenerator->generate();
    auto fromBranch = makeProject(
        std::move(inputStage),
        planNodeId,
        inputSlot,
        makeFunction("getField", makeVariable(inputDocumentSlot), makeConstant(fieldName)));

    if (isLeafField && mode == LeafTraversalMode::kDoNotTraverseLeaf) {
        // 'makePredicate' in this mode must return valid state, not just plain boolean value. So
        // there is no need to wrap it in '_context->stateHelper.makePredicateCombinator'.
        return makePredicate(inputSlot, std::move(fromBranch));
    }

    // Input slot for the inner branch of traverse stage is the same as the input slot holding the
    // array.
    auto innerInputSlot = inputSlot;
    auto traverseInputSlot = inputSlot;

    // Some of MQL expressions need to check predicate not only for each of the array elements, but
    // also for the whole array. Predicate tree is located in the inner branch of the traverse stage
    // created below. To avoid generating predicate tree two times, we force traverse to be executed
    // two times: first to iterate array elements and second to run the predicate tree against whole
    // array.
    // To achive this, we create union stage in the 'from' branch of traverse. This union stage
    // sets the input slot of the traverse stage - 'traverseInputSlot'. Union returns ADVANCED
    // two times, forcing traverse to be executed two times with different inputs:
    //  - First time union returns ADVANCED, 'traverseInputSlot' is set to the input array, stored
    //    in 'inputSlot'. Traverse stage iterates over array elements (if any) and checks the
    //    predicate for each of them.
    //  - Second time union returns ADVANCED, 'traverseInputSlot' is set to Nothing. In this case,
    //    traverse stage executes predicate only once.
    // Since 'from' branch of traverse has union stage, we save current 'fromBranch' to use for
    // loop join stage later.
    EvalStage innerBranch;
    EvalStage loopJoinFromBranch;
    if (needsArrayCheck) {
        loopJoinFromBranch = std::move(fromBranch);

        auto buildUnionBranch = [&](std::unique_ptr<sbe::EExpression> arrayExpr) {
            auto currentArraySlot = slotIdGenerator->generate();
            auto branch = makeProject(makeLimitCoScanStage(planNodeId),
                                      planNodeId,
                                      currentArraySlot,
                                      std::move(arrayExpr));
            return std::make_pair(sbe::makeSV(currentArraySlot), std::move(branch));
        };

        auto [checkArrayElementsSlots, checkArrayElementsStage] =
            buildUnionBranch(makeVariable(inputSlot));

        auto [checkWholeArraySlots, checkWholeArrayStage] =
            buildUnionBranch(makeConstant(sbe::value::TypeTags::Nothing, 0));

        traverseInputSlot = slotIdGenerator->generate();
        fromBranch = makeUnion(
            makeVector(std::move(checkArrayElementsStage), std::move(checkWholeArrayStage)),
            makeVector(std::move(checkArrayElementsSlots), std::move(checkWholeArraySlots)),
            sbe::makeSV(traverseInputSlot),
            planNodeId);
    }

    boost::optional<sbe::value::SlotId> isTraverseInputArraySlot;
    if (needsArrayCheck || !isLeafField || stateHelper.stateContainsValue()) {
        isTraverseInputArraySlot = slotIdGenerator->generate();
        fromBranch = makeProject(
            std::move(fromBranch),
            planNodeId,
            *isTraverseInputArraySlot,
            makeFillEmptyFalse(makeFunction("isArray", makeVariable(traverseInputSlot))));
    }

    // If current input to the traverse stage is an array, this means that we are currently
    // checking the predicate against each of the array elements. 'traverseInputSlot', holding
    // current array element, should be passed to the predicate.
    // If current input to the traverse stage is not an array, this could mean two things:
    //  - Value in the 'inputSlot' is not the array
    //  - We are checking the predicate against the whole array
    // In both cases, 'inputSlot' should be passed to the predicate.
    if (needsArrayCheck) {
        innerInputSlot = slotIdGenerator->generate();
        innerBranch = makeProject(std::move(innerBranch),
                                  planNodeId,
                                  innerInputSlot,
                                  sbe::makeE<sbe::EIf>(makeVariable(*isTraverseInputArraySlot),
                                                       makeVariable(traverseInputSlot),
                                                       makeVariable(inputSlot)));
    }

    // For the non leaf nodes we insert a filter that allows the nested getField only for objects.
    // But only if the outer value is an array. This is relevant in this example: given 2 documents
    // {a:10} and {a:[10]} the filer {'a.b':null} returns the first document but not the second.
    // Without the filter we'd try to traverse 'a', and in both cases the inner side of the
    // 'traverse' would get the value '10'. However, in the first case we'd try to apply getField()
    // to a standalone scalar, which would return a missing field, which is equal to null, whilst in
    // a second case to a scalar which is an array element. According to the legacy implementation,
    // this is not allowed and we shouldn't try to do a nesting path traversal of the array
    // elements, unless an element is an object.
    if (!isLeafField) {
        innerBranch =
            makeFilter<false>(std::move(innerBranch),
                              makeBinaryOp(sbe::EPrimBinary::logicOr,
                                           makeNot(makeVariable(*isTraverseInputArraySlot)),
                                           makeFunction("isObject", makeVariable(innerInputSlot))),
                              planNodeId);
    }

    // Generate the 'in' branch for the TraverseStage that we're about to construct.
    EvalExpr innerExpr;
    std::tie(innerExpr, innerBranch) = isLeafField
        // Base case: Evaluate the predicate. Predicate returns boolean value, we need to convert it
        // to state using 'stateHelper.makePredicateCombinator'.
        ? stateHelper.makePredicateCombinator(makePredicate(innerInputSlot, std::move(innerBranch)))
        // Recursive case.
        : generatePathTraversal(std::move(innerBranch),
                                innerInputSlot,
                                fp,
                                level + 1,
                                planNodeId,
                                slotIdGenerator,
                                frameIdGenerator,
                                makePredicate,
                                mode,
                                stateHelper);

    if (stateHelper.stateContainsValue()) {
        // The expression below checks if input is an array. In this case it returns initial state.
        // This value will be the first one to be stored in 'traverseOutputSlot'. On the subsequent
        // iterations 'traverseOutputSlot' is updated according to fold expression.
        // If input is not array, expression below simply assigns state from the predicate to the
        // 'innerResultSlot'.
        // If state does not containy any value apart from boolean, we do not need to perform this
        // check.
        innerExpr =
            makeLocalBind(frameIdGenerator,
                          [&](sbe::EVariable state) {
                              return sbe::makeE<sbe::EIf>(
                                  makeVariable(*isTraverseInputArraySlot),
                                  stateHelper.makeInitialState(stateHelper.getBool(state.clone())),
                                  state.clone());
                          },
                          innerExpr.extractExpr());
    }

    sbe::value::SlotId innerResultSlot;
    std::tie(innerResultSlot, innerBranch) =
        projectEvalExpr(std::move(innerExpr),
                        std::move(innerBranch),  // NOLINT(bugprone-use-after-move)
                        planNodeId,
                        slotIdGenerator);

    // Generate the traverse stage for the current nested level. There are several cases covered
    // during this phase:
    //  1. If input is not an array, value from 'in' branch is returned (see comment for the 'in'
    //     branch construction).
    //  2. If input is an array of size 1, fold expression is never executed. 'in' branch returns
    //     initial state, paired with false value if predicate evaluates to false and true value
    //     otherwise.
    //  3. If input is an array of size larger than 1 and predicate does not evaluate to true on the
    //     first array element, fold expression is executed at least once. See comments for
    //     respective implementation of 'FilterStateHelper::makeTraverseCombinator' for details.
    auto traverseOutputSlot = slotIdGenerator->generate();
    auto outputStage = stateHelper.makeTraverseCombinator(
        std::move(fromBranch),
        std::move(innerBranch),  // NOLINT(bugprone-use-after-move)
        traverseInputSlot,
        traverseOutputSlot,
        innerResultSlot,
        planNodeId,
        frameIdGenerator);

    // If traverse stage was not executed at all (empty input array), 'traverseOutputSlot' contains
    // Nothing. In this case we have not found matching element, so we simply return false value.
    auto resultExpr =
        makeFunction("fillEmpty", makeVariable(traverseOutputSlot), stateHelper.makeState(false));

    if (!needsArrayCheck) {
        return {std::move(resultExpr), std::move(outputStage)};
    }

    auto outputSlot = slotIdGenerator->generate();
    outputStage =
        makeProject(std::move(outputStage), planNodeId, outputSlot, std::move(resultExpr));

    // In case predicate needs to be checked both for each of the array elements and for whole
    // array, traverse stage created above will return ADVANCED two times. To handle that, we
    // construct the following tree:
    //
    //   nlj
    //   left
    //       <'inputStage' and extracting current field value into 'inputSlot'>
    //   right
    //       limit 1
    //       filter {!isTraverseInputArraySlot || outputSlot}
    //       <traverse stage created above>
    //
    // Let iterate over each part of the tree:
    //  - Loop join stage is created to hold all stages which usually go into the 'from' branch of
    //    traverse stage. This includes 'inputStage' and project stage to extract current field
    //    value.
    //  - Filter stage ensures that tree below it returns ADVANCED only if the predicate matched
    //    one of the array elements or the whole array.
    //  - Limit-1 stage ensures short-circuiting. If one of the array elements matched the
    //    predicate, filter stage below it returns ADVANCED and we do not execute the predicate
    //    for the whole array.
    //
    // To better understand the predicate of the filter stage, let us take a look how the resulting
    // tree behaves for various 'inputSlot' values. 'inputSlot' can be:
    //  - Array. In this case traverse stage will be executed twice:
    //   1. 'isTraverseInputArraySlot = true', filter will pass only if 'outputSlot = true', which
    //      means predicate returned true for one of the array elements.
    //   2. 'isTraverseInputArray = false' (since second time traverse input is Nothing), filter
    //      will always pass. Even though predicate may not match the whole array, we need to return
    //      something to the stage above us.
    // - Not array. In this case traverse stage will be executed once:
    //   1. 'isTraverseInputArray = false', filter will always pass.
    //   2. Will never happen because of limit-1 stage on top.
    outputStage = makeFilter<false>(std::move(outputStage),
                                    makeBinaryOp(sbe::EPrimBinary::logicOr,
                                                 makeNot(makeVariable(*isTraverseInputArraySlot)),
                                                 stateHelper.getBool(outputSlot)),
                                    planNodeId);

    outputStage = makeLimitSkip(std::move(outputStage), planNodeId, 1);

    outputStage = makeLoopJoin(std::move(loopJoinFromBranch), std::move(outputStage), planNodeId);

    return {outputSlot, std::move(outputStage)};
}

/**
 * Given a field path 'path' and a predicate 'makePredicate', this function generates an SBE tree
 * that will evaluate the predicate on the field path. When 'path' is not empty string (""), this
 * function generates a sequence of nested traverse operators to traverse the field path and it uses
 * 'makePredicate' to generate an SBE expression for evaluating the predicate on individual value.
 * When 'path' is empty, this function simply uses 'makePredicate' to generate an SBE expression for
 * evaluating the predicate on a single value.
 */
void generatePredicate(MatchExpressionVisitorContext* context,
                       const FieldRef* path,
                       MakePredicateFn makePredicate,
                       LeafTraversalMode mode,
                       bool useCombinator = true) {
    auto& frame = context->evalStack.topFrame();

    auto&& [expr, stage] = [&]() {
        if (frame.data().inputSlot) {
            if (path && !path->empty()) {
                return generatePathTraversal(frame.extractStage(),
                                             *frame.data().inputSlot,
                                             *path,
                                             0,
                                             context->planNodeId,
                                             context->state.slotIdGenerator,
                                             context->state.frameIdGenerator,
                                             makePredicate,
                                             mode,
                                             context->stateHelper);
            } else {
                // If matchExpr's parent is a ElemMatchValueMatchExpression, then
                // matchExpr()->fieldRef() will be nullptr. In this case, 'inputSlot' will be a
                // "correlated slot" that holds the value of the ElemMatchValueMatchExpression's
                // field path, and we should apply the predicate directly on 'inputSlot' without
                // array traversal.
                auto result = makePredicate(*frame.data().inputSlot, frame.extractStage());
                if (useCombinator) {
                    return context->stateHelper.makePredicateCombinator(std::move(result));
                }
                return result;
            }
        } else {
            // If an input slot for the current frame is not defined, then we must generating a
            // filter predicate for an index scan. In this case we don't need to perform any complex
            // path traversal but rather evaluate the predicate directly on the input slot for the
            // current field path - the index scan will extract the value for this field path and
            // will store it in a corresponding slot in the 'indexKeySlots' map.

            tassert(5273402, "Field path cannot be empty for an index filter", path);

            auto it = context->indexKeySlots.find(path->dottedField());
            tassert(5273403,
                    str::stream() << "Unknown field path in index filter: " << path->dottedField(),
                    it != context->indexKeySlots.end());

            auto result = makePredicate(it->second, frame.extractStage());
            if (useCombinator) {
                return context->stateHelper.makePredicateCombinator(std::move(result));
            }
            return result;
        }
    }();

    frame.setStage(std::move(stage));
    frame.pushExpr(std::move(expr));
}

/**
 * Generates a path traversal SBE plan stage sub-tree for matching arrays with '$size'. Applies
 * an extra project on top of the sub-tree to filter based on user provided value.
 */
void generateArraySize(MatchExpressionVisitorContext* context,
                       const SizeMatchExpression* matchExpr) {
    int size = matchExpr->getData();
    // If there's an "inputParamId" in 'matchExpr' meaning this expr got parameterized, we can
    // register a SlotId for it and use the slot directly.
    boost::optional<sbe::value::SlotId> inputParamSlotId;
    if (auto inputParam = matchExpr->getInputParamId()) {
        inputParamSlotId = context->state.registerInputParamSlot(*inputParam);
    }

    auto makePredicate = [&](sbe::value::SlotId inputSlot,
                             EvalStage inputStage) -> EvalExprStagePair {
        // Generate a traverse that projects the integer value 1 for each element in the array and
        // then sums up the 1's, resulting in the count of elements in the array.
        auto innerSlot = context->state.slotId();
        auto innerBranch =
            makeProject(EvalStage{},
                        context->planNodeId,
                        innerSlot,
                        sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64,
                                                   sbe::value::bitcastFrom<int64_t>(1)));

        auto traverseSlot = context->state.slotId();
        auto traverseStage = makeTraverse(EvalStage{},
                                          std::move(innerBranch),
                                          inputSlot,
                                          traverseSlot,
                                          innerSlot,
                                          makeBinaryOp(sbe::EPrimBinary::add,
                                                       sbe::makeE<sbe::EVariable>(traverseSlot),
                                                       sbe::makeE<sbe::EVariable>(innerSlot)),
                                          nullptr,
                                          context->planNodeId,
                                          1);

        auto sizeExpr = inputParamSlotId ? makeVariable(*inputParamSlotId)
                                         : makeConstant(sbe::value::TypeTags::NumberInt64,
                                                        sbe::value::bitcastFrom<int64_t>(size));
        // If the traversal result was not Nothing, compare it to the user provided value. If the
        // traversal result was Nothing, that means the array was empty, so replace Nothing with 0
        // and compare it to the user provided value.
        auto sizeOutput = makeBinaryOp(
            sbe::EPrimBinary::eq,
            std::move(sizeExpr),
            sbe::makeE<sbe::EIf>(makeFunction("exists", sbe::makeE<sbe::EVariable>(traverseSlot)),
                                 sbe::makeE<sbe::EVariable>(traverseSlot),
                                 sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64,
                                                            sbe::value::bitcastFrom<int64_t>(0))));

        std::vector<EvalExprStagePair> branches;

        // Check that the thing we are about traverse is indeed an array.
        branches.emplace_back(
            makeFillEmptyFalse(makeFunction("isArray", sbe::makeE<sbe::EVariable>(inputSlot))),
            EvalStage{});

        branches.emplace_back(std::move(sizeOutput), std::move(traverseStage));

        auto [opOutput, opStage] = generateShortCircuitingLogicalOp(sbe::EPrimBinary::logicAnd,
                                                                    std::move(branches),
                                                                    context->planNodeId,
                                                                    context->state.slotIdGenerator,
                                                                    BooleanStateHelper{});

        inputStage = makeLoopJoin(std::move(inputStage), std::move(opStage), context->planNodeId);

        return {context->stateHelper.makeState(opOutput.extractExpr()), std::move(inputStage)};
    };

    generatePredicate(context,
                      matchExpr->fieldRef(),
                      std::move(makePredicate),
                      LeafTraversalMode::kDoNotTraverseLeaf);
}

/**
 * Generates a path traversal SBE plan stage sub-tree which implements the comparison match
 * expression 'expr'. The comparison itself executes using the given 'binaryOp'.
 */
void generateComparison(MatchExpressionVisitorContext* context,
                        const ComparisonMatchExpression* expr,
                        sbe::EPrimBinary::Op binaryOp) {
    auto makePredicate = [context, expr, binaryOp](sbe::value::SlotId inputSlot,
                                                   EvalStage inputStage) -> EvalExprStagePair {
        const auto& rhs = expr->getData();
        auto [tagView, valView] = sbe::bson::convertFrom<true>(
            rhs.rawdata(), rhs.rawdata() + rhs.size(), rhs.fieldNameSize() - 1);

        // Most commonly the comparison does not do any kind of type conversions (i.e. 12 > "10"
        // does not evaluate to true as we do not try to convert a string to a number). Internally,
        // SBE returns Nothing for mismatched types.
        // However, there is a wrinkle with MQL (and there always is one). We can compare any type
        // to MinKey or MaxKey type and expect a true/false answer.
        if (tagView == sbe::value::TypeTags::MinKey) {
            switch (binaryOp) {
                case sbe::EPrimBinary::eq:
                case sbe::EPrimBinary::neq:
                    break;
                case sbe::EPrimBinary::greater: {
                    return {makeFillEmptyFalse(
                                makeNot(makeFunction("isMinKey", makeVariable(inputSlot)))),
                            std::move(inputStage)};
                }
                case sbe::EPrimBinary::greaterEq: {
                    return {makeFunction("exists", makeVariable(inputSlot)), std::move(inputStage)};
                }
                case sbe::EPrimBinary::less: {
                    return {makeConstant(sbe::value::TypeTags::Boolean, false),
                            std::move(inputStage)};
                }
                case sbe::EPrimBinary::lessEq: {
                    return {makeFillEmptyFalse(makeFunction("isMinKey", makeVariable(inputSlot))),
                            std::move(inputStage)};
                }
                default:
                    break;
            }
        } else if (tagView == sbe::value::TypeTags::MaxKey) {
            switch (binaryOp) {
                case sbe::EPrimBinary::eq:
                case sbe::EPrimBinary::neq:
                    break;
                case sbe::EPrimBinary::greater: {
                    return {makeConstant(sbe::value::TypeTags::Boolean, false),
                            std::move(inputStage)};
                }
                case sbe::EPrimBinary::greaterEq: {
                    return {makeFillEmptyFalse(makeFunction("isMaxKey", makeVariable(inputSlot))),
                            std::move(inputStage)};
                }
                case sbe::EPrimBinary::less: {
                    return {makeFillEmptyFalse(
                                makeNot(makeFunction("isMaxKey", makeVariable(inputSlot)))),
                            std::move(inputStage)};
                }
                case sbe::EPrimBinary::lessEq: {
                    return {makeFunction("exists", makeVariable(inputSlot)), std::move(inputStage)};
                }
                default:
                    break;
            }
        } else if (tagView == sbe::value::TypeTags::Null) {
            // When comparing to null we have to consider missing and undefined.
            auto inputExpr = buildMultiBranchConditional(
                CaseValuePair{generateNullOrMissing(sbe::EVariable(inputSlot)),
                              makeConstant(sbe::value::TypeTags::Null, 0)},
                makeVariable(inputSlot));

            return {makeFillEmptyFalse(makeBinaryOp(binaryOp,
                                                    std::move(inputExpr),
                                                    makeConstant(sbe::value::TypeTags::Null, 0),
                                                    context->state.data->env)),
                    std::move(inputStage)};
        } else if (sbe::value::isNaN(tagView, valView)) {
            // Construct an expression to perform a NaN check.
            switch (binaryOp) {
                case sbe::EPrimBinary::eq:
                case sbe::EPrimBinary::greaterEq:
                case sbe::EPrimBinary::lessEq:
                    // If 'rhs' is NaN, then return whether the lhs is NaN.
                    return {makeFillEmptyFalse(makeFunction("isNaN", makeVariable(inputSlot))),
                            std::move(inputStage)};
                case sbe::EPrimBinary::less:
                case sbe::EPrimBinary::greater:
                    // Always return false for non-equality operators.
                    return {makeConstant(sbe::value::TypeTags::Boolean,
                                         sbe::value::bitcastFrom<bool>(false)),
                            std::move(inputStage)};
                default:
                    tasserted(5449400,
                              str::stream() << "Could not construct expression for comparison op "
                                            << expr->toString());
            }
        }

        auto valExpr = [&](sbe::value::TypeTags typeTag,
                           sbe::value::Value value) -> std::unique_ptr<sbe::EExpression> {
            if (auto inputParam = expr->getInputParamId()) {
                return makeVariable(context->state.registerInputParamSlot(*inputParam));
            }
            auto [tag, val] = sbe::value::copyValue(typeTag, value);
            return makeConstant(tag, val);
        }(tagView, valView);

        return {
            makeFillEmptyFalse(makeBinaryOp(
                binaryOp, makeVariable(inputSlot), std::move(valExpr), context->state.data->env)),
            std::move(inputStage)};
    };

    // A 'kArrayAndItsElements' traversal mode matches the following semantics: when the path we are
    // comparing is a path to an array, the comparison is considered true if it evaluates to true
    // for the array itself or for any of the array's elements.
    // However, we use 'kArrayElementsOnly' for the general case, because the comparison with the
    // array will almost always be false. There are two exceptions:
    // 1) when the 'rhs' operand is an array and
    // 2) when the 'rhs' operand is MinKey or MaxKey.
    // In the former case, the comparison we would skip by using 'kArrayElementsOnly' mode is an
    // array-to-array comparison that can return true. In the latter case, we are avoiding a
    // potential bug where traversing the path to the empty array ([]) would prevent _any_
    // comparison, meaning a comparison like {$gt: MinKey} would return false.
    const auto& rhs = expr->getData();
    const auto checkWholeArray = rhs.type() == BSONType::Array || rhs.type() == BSONType::MinKey ||
        rhs.type() == BSONType::MaxKey;
    const auto traversalMode = checkWholeArray ? LeafTraversalMode::kArrayAndItsElements
                                               : LeafTraversalMode::kArrayElementsOnly;
    generatePredicate(context, expr->fieldRef(), std::move(makePredicate), traversalMode);
}

/**
 * Generates and pushes a constant boolean expression for either alwaysTrue or alwaysFalse.
 */
void generateAlwaysBoolean(MatchExpressionVisitorContext* context, bool value) {
    auto& frame = context->evalStack.topFrame();
    frame.pushExpr(context->stateHelper.makeState(value));
}

/**
 * Generates a SBE plan stage sub-tree which implements the bitwise match expression 'expr'. The
 * various bit test expressions accept a numeric, BinData or position list bitmask. Here we handle
 * building an EExpression for both the numeric and BinData or position list forms of the bitmask.
 */
void generateBitTest(MatchExpressionVisitorContext* context,
                     const BitTestMatchExpression* expr,
                     const sbe::BitTestBehavior& bitOp) {
    auto makePredicate = [expr, bitOp, context](sbe::value::SlotId inputSlot,
                                                EvalStage inputStage) -> EvalExprStagePair {
        // If there's an "inputParamId" in this expr meaning this expr got parameterized, we can
        // register a SlotId for it and use the slot directly.
        std::unique_ptr<sbe::EExpression> bitPosExpr = [&]() -> std::unique_ptr<sbe::EExpression> {
            if (auto bitPosParamId = expr->getBitPositionsParamId()) {
                auto bitPosSlotId = context->state.registerInputParamSlot(*bitPosParamId);
                return makeVariable(bitPosSlotId);
            } else {
                auto [bitPosTag, bitPosVal] = convertBitTestBitPositions(expr);
                return makeConstant(bitPosTag, bitPosVal);
            }
        }();

        // An EExpression for the BinData and position list for the binary case of
        // BitTestMatchExpressions. This function will be applied to values carrying BinData
        // elements.
        auto binaryBitTestExpr = makeFunction(
            "bitTestPosition"_sd,
            std::move(bitPosExpr),
            makeVariable(inputSlot),
            makeConstant(sbe::value::TypeTags::NumberInt32, static_cast<int32_t>(bitOp)));

        // Build An EExpression for the numeric bitmask case. The AllSet case tests if (mask &
        // value) == mask, and AllClear case tests if (mask & value) == 0. The AnyClear and
        // AnySet cases are the negation of the AllSet and AllClear cases, respectively.
        auto numericBitTestFnName = [&]() {
            if (bitOp == sbe::BitTestBehavior::AllSet || bitOp == sbe::BitTestBehavior::AnyClear) {
                return "bitTestMask"_sd;
            }
            if (bitOp == sbe::BitTestBehavior::AllClear || bitOp == sbe::BitTestBehavior::AnySet) {
                return "bitTestZero"_sd;
            }
            MONGO_UNREACHABLE_TASSERT(5610200);
        }();

        // We round NumberDecimal values to the nearest integer to match the classic execution
        // engine's behavior for now. Note that this behavior is _not_ consistent with MongoDB's
        // documentation. At some point, we should consider removing this call to round() to make
        // SBE's behavior consistent with MongoDB's documentation.
        auto numericBitTestInputExpr = sbe::makeE<sbe::EIf>(
            makeFunction("typeMatch",
                         makeVariable(inputSlot),
                         makeConstant(sbe::value::TypeTags::NumberInt64,
                                      sbe::value::bitcastFrom<int64_t>(
                                          getBSONTypeMask(sbe::value::TypeTags::NumberDecimal)))),
            makeFunction("round"_sd, makeVariable(inputSlot)),
            makeVariable(inputSlot));

        std::unique_ptr<sbe::EExpression> bitMaskExpr = [&]() -> std::unique_ptr<sbe::EExpression> {
            if (auto bitMaskParamId = expr->getBitMaskParamId()) {
                auto bitMaskSlotId = context->state.registerInputParamSlot(*bitMaskParamId);
                return makeVariable(bitMaskSlotId);
            } else {
                return makeConstant(sbe::value::TypeTags::NumberInt64, expr->getBitMask());
            }
        }();
        // Convert the value to a 64-bit integer, and then pass the converted value along with the
        // mask to the appropriate bit-test function. If the value cannot be losslessly converted
        // to a 64-bit integer, this expression will return Nothing.
        auto numericBitTestExpr =
            makeFunction(numericBitTestFnName,
                         std::move(bitMaskExpr),
                         sbe::makeE<sbe::ENumericConvert>(std::move(numericBitTestInputExpr),
                                                          sbe::value::TypeTags::NumberInt64));

        // For the AnyClear and AnySet cases, negate the output of the bit-test function.
        if (bitOp == sbe::BitTestBehavior::AnyClear || bitOp == sbe::BitTestBehavior::AnySet) {
            numericBitTestExpr = makeNot(std::move(numericBitTestExpr));
        }

        // numericBitTestExpr might produce Nothing, so we wrap it with makeFillEmptyFalse().
        return {sbe::makeE<sbe::EIf>(makeFunction("isBinData"_sd, makeVariable(inputSlot)),
                                     std::move(binaryBitTestExpr),
                                     makeFillEmptyFalse(std::move(numericBitTestExpr))),
                std::move(inputStage)};
    };

    generatePredicate(
        context, expr->fieldRef(), std::move(makePredicate), LeafTraversalMode::kArrayElementsOnly);
}

// Each logical expression child is evaluated in a separate EvalFrame. Set up a new EvalFrame with a
// limit-1/coscan tree.
void pushFrameForLogicalExpressionChild(MatchExpressionVisitorContext* context,
                                        size_t numChildren) {
    if (numChildren <= 1) {
        // For logical expressions with no children, we return constant (handled in the
        // post-visitor). For expressions with 1 child, we evaluate the child within the current
        // EvalFrame.
        return;
    }

    const auto& frame = context->evalStack.topFrame();
    context->evalStack.emplaceFrame(EvalStage{}, frame.data().inputSlot);
}

// Build specified logical expression with branches stored on stack.
void buildLogicalExpression(sbe::EPrimBinary::Op op,
                            size_t numChildren,
                            MatchExpressionVisitorContext* context) {
    if (numChildren == 0) {
        // If logical expression does not have any children, constant is returned.
        generateAlwaysBoolean(context, op == sbe::EPrimBinary::logicAnd);
        return;
    } else if (numChildren == 1) {
        // For expressions with 1 child, do nothing and return. The post-visitor for the child
        // expression has already done all the necessary work.
        return;
    }

    // Move the children's outputs off of the evalStack into a vector in preparation for
    // calling generateShortCircuitingLogicalOp().
    std::vector<EvalExprStagePair> branches;
    for (size_t i = 0; i < numChildren; ++i) {
        auto [expr, stage] = context->evalStack.popFrame();
        branches.emplace_back(std::move(expr), std::move(stage));
    }
    std::reverse(branches.begin(), branches.end());

    auto& frame = context->evalStack.topFrame();
    auto&& [expr, opStage] = generateShortCircuitingLogicalOp(op,
                                                              std::move(branches),
                                                              context->planNodeId,
                                                              context->state.slotIdGenerator,
                                                              context->stateHelper);
    frame.pushExpr(std::move(expr));

    // Join frame.stage with opStage.
    frame.setStage(makeLoopJoin(frame.extractStage(), std::move(opStage), context->planNodeId));
}

/**
 * Helper to use for 'makePredicate' argument of 'generatePredicate' function for $elemMatch
 * expressions.
 */
EvalExprStagePair elemMatchMakePredicate(MatchExpressionVisitorContext* context,
                                         sbe::value::SlotId filterSlot,
                                         EvalStage& filterStage,
                                         sbe::value::SlotId childInputSlot,
                                         sbe::value::SlotId inputSlot,
                                         EvalStage inputStage) {
    // The 'filterStage' subtree was generated to read from 'childInputSlot', based on
    // the assumption that 'childInputSlot' is some correlated slot that will be made
    // available by childStages's parent. We add a projection here to 'inputStage' to
    // feed 'inputSlot' into 'childInputSlot'.
    auto isInputArray = context->state.slotId();
    auto fromBranch = makeProject(std::move(inputStage),
                                  context->planNodeId,
                                  childInputSlot,
                                  sbe::makeE<sbe::EVariable>(inputSlot),
                                  isInputArray,
                                  makeFunction("isArray", sbe::makeE<sbe::EVariable>(inputSlot)));

    auto [innerResultSlot, innerBranch] = [&]() -> std::pair<sbe::value::SlotId, EvalStage> {
        if (!context->stateHelper.stateContainsValue()) {
            return {filterSlot, std::move(filterStage)};
        }

        auto resultSlot = context->state.slotId();
        return {resultSlot,
                makeProject(std::move(filterStage),
                            context->planNodeId,
                            resultSlot,
                            context->stateHelper.makeInitialState(
                                context->stateHelper.getBool(filterSlot)))};
    }();

    innerBranch = makeFilter<true>(
        std::move(innerBranch), sbe::makeE<sbe::EVariable>(isInputArray), context->planNodeId);

    // Generate the traverse.
    auto traverseSlot = context->state.slotId();
    auto traverseStage = context->stateHelper.makeTraverseCombinator(
        std::move(fromBranch),
        std::move(innerBranch),  // NOLINT(bugprone-use-after-move)
        childInputSlot,
        traverseSlot,
        innerResultSlot,
        context->planNodeId,
        context->state.frameIdGenerator);

    // There are some cases where 'traverseOutputSlot' gets set to Nothing when TraverseStage
    // doesn't match anything. One  example of when this happens is when innerBranch->getNext()
    // returns EOF every time it is called by TraverseStage. In these cases $elemMatch should return
    // false instead of Nothing.
    auto projectExpr = makeFunction("fillEmpty",
                                    sbe::makeE<sbe::EVariable>(traverseSlot),
                                    context->stateHelper.makeState(false));

    return {std::move(projectExpr), std::move(traverseStage)};
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
        if (expr == _context->topLevelAnd) {
            // Usually, we implement AND expression using limit-1/union tree. Each branch of a union
            // stage represents AND's argument. For top-level AND we apply an optimization that
            // allows us to get rid of limit-1/union tree.
            // Firstly, we add filter stage on top of tree for each of AND's arguments. This ensures
            // that respective tree does not return ADVANCED if argument evaluates to false.
            // Secondly, we place trees of AND's arguments on top of each other. This guarantees
            // that the whole resulting tree for AND does not return ADVANCED if one of arguments
            // did not returned ADVANCED (e.g. evaluated to false).
            // First step is performed in 'MatchExpressionInVisitor' and
            // 'MatchExpressionPostVisitor'. Second step is achieved by evaluating each child within
            // one EvalFrame, so that each child builds directly on top of
            // '_context->evalStack.topFrame().extractStage()'.
            return;
        }

        // For non-top-level $and's, we evaluate each child in its own EvalFrame.
        pushFrameForLogicalExpressionChild(_context, expr->numChildren());
    }

    void visit(const BitsAllClearMatchExpression* expr) final {}
    void visit(const BitsAllSetMatchExpression* expr) final {}
    void visit(const BitsAnyClearMatchExpression* expr) final {}
    void visit(const BitsAnySetMatchExpression* expr) final {}

    void visit(const ElemMatchObjectMatchExpression* matchExpr) final {
        // ElemMatchObjectMatchExpression is guaranteed to always have exactly 1 child
        invariant(matchExpr->numChildren() == 1);

        // We evaluate $elemMatch's child in a new EvalFrame. For the child's EvalFrame, we set the
        // 'stage' field to be a null tree, and we set the 'inputSlot' field to be a newly allocated
        // slot (childInputSlot). childInputSlot is a "correlated slot" that will be set up later
        // (handled in the post-visitor).
        auto childInputSlot = _context->state.slotId();
        _context->evalStack.emplaceFrame(EvalStage{}, childInputSlot);
    }

    void visit(const ElemMatchValueMatchExpression* matchExpr) final {
        invariant(matchExpr->numChildren() >= 1);

        // We evaluate each child in its own EvalFrame. Set up a new EvalFrame with a null tree
        // for the first child. For all of the children's EvalFrames, we set the 'inputSlot' field
        // to 'childInputSlot'. childInputSlot is a "correlated slot" that will be set up later in
        // the post-visitor (childInputSlot will be the correlated parameter of a TraverseStage).
        auto childInputSlot = _context->state.slotId();
        _context->evalStack.emplaceFrame(EvalStage{}, childInputSlot);
    }

    void visit(const EqualityMatchExpression* expr) final {}
    void visit(const ExistsMatchExpression* expr) final {}
    void visit(const ExprMatchExpression* expr) final {}
    void visit(const GTEMatchExpression* expr) final {}
    void visit(const GTMatchExpression* expr) final {}
    void visit(const GeoMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const GeoNearMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InMatchExpression* expr) final {}
    void visit(const InternalBucketGeoWithinMatchExpression* expr) final {}
    void visit(const InternalExprEqMatchExpression* expr) final {}
    void visit(const InternalExprGTMatchExpression* expr) final {}
    void visit(const InternalExprGTEMatchExpression* expr) final {}
    void visit(const InternalExprLTMatchExpression* expr) final {}
    void visit(const InternalExprLTEMatchExpression* expr) final {}
    void visit(const InternalSchemaAllElemMatchFromIndexMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InternalSchemaAllowedPropertiesMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InternalSchemaBinDataEncryptedTypeExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InternalSchemaBinDataFLE2EncryptedTypeExpression* expr) final {
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
        pushFrameForLogicalExpressionChild(_context, expr->numChildren());
    }

    void visit(const NotMatchExpression* expr) final {
        invariant(expr->numChildren() == 1);
    }

    void visit(const OrMatchExpression* expr) final {
        pushFrameForLogicalExpressionChild(_context, expr->numChildren());
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
    void visit(const WhereMatchExpression* expr) final {}
    void visit(const WhereNoOpMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const EncryptedBetweenMatchExpression* expr) final {
        unsupportedExpression(expr);
    }

private:
    void unsupportedExpression(const MatchExpression* expr) const {
        // We're guaranteed to not fire this assertion by implementing a mechanism in the upper
        // layer which directs the query to the classic engine when an unsupported expression
        // appears.
        tasserted(4822878,
                  str::stream() << "Unsupported match expression in SBE stage builder: "
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
        if (expr == _context->topLevelAnd) {
            // For a top-level $and with no children, do nothing and return. For top-level $and's
            // with at least one, we evaluate each child within the current EvalFrame.
            if (expr->numChildren() >= 1) {
                // Process the output of the last child.
                if (_context->stateHelper.stateContainsValue()) {
                    projectCurrentExprToOutputSlot(_context);
                }

                auto& frame = _context->evalStack.topFrame();
                invariant(frame.exprsCount() > 0);
                frame.setStage(
                    makeFilter<false>(frame.extractStage(),
                                      _context->stateHelper.getBool(frame.popExpr().extractExpr()),
                                      _context->planNodeId));
            }
            return;
        }

        buildLogicalExpression(sbe::EPrimBinary::logicAnd, expr->numChildren(), _context);
    }

    void visit(const BitsAllClearMatchExpression* expr) final {
        generateBitTest(_context, expr, sbe::BitTestBehavior::AllClear);
    }

    void visit(const BitsAllSetMatchExpression* expr) final {
        generateBitTest(_context, expr, sbe::BitTestBehavior::AllSet);
    }

    void visit(const BitsAnyClearMatchExpression* expr) final {
        generateBitTest(_context, expr, sbe::BitTestBehavior::AnyClear);
    }

    void visit(const BitsAnySetMatchExpression* expr) final {
        generateBitTest(_context, expr, sbe::BitTestBehavior::AnySet);
    }

    void visit(const ElemMatchObjectMatchExpression* matchExpr) final {
        using namespace std::placeholders;
        // ElemMatchObjectMatchExpression is guaranteed to always have exactly 1 child
        invariant(matchExpr->numChildren() == 1);

        // Extract the input slot, the output, and the stage from of the child's EvalFrame, and
        // remove the child's EvalFrame from the stack.
        tassert(5273405,
                "Eval frame's input slot is not defined",
                _context->evalStack.topFrame().data().inputSlot);
        auto childInputSlot = *_context->evalStack.topFrame().data().inputSlot;
        auto [filterSlot, filterStage] = [&]() {
            auto [expr, stage] = _context->evalStack.popFrame();
            auto [predicateSlot, predicateStage] = projectEvalExpr(std::move(expr),
                                                                   std::move(stage),
                                                                   _context->planNodeId,
                                                                   _context->state.slotIdGenerator);

            auto isObjectOrArrayExpr =
                makeBinaryOp(sbe::EPrimBinary::logicOr,
                             makeFunction("isObject", sbe::makeE<sbe::EVariable>(childInputSlot)),
                             makeFunction("isArray", sbe::makeE<sbe::EVariable>(childInputSlot)));
            predicateStage = makeFilter<true>(
                std::move(predicateStage), std::move(isObjectOrArrayExpr), _context->planNodeId);
            return std::make_pair(predicateSlot, std::move(predicateStage));
        }();

        // We're using 'kDoNotTraverseLeaf' traverse mode, so we're guaranteed that 'makePredcate'
        // will only be called once, so it's safe to bind the reference to 'filterStage' subtree
        // here.
        auto makePredicate = std::bind(&elemMatchMakePredicate,
                                       _context,
                                       filterSlot,
                                       std::ref(filterStage),
                                       childInputSlot,
                                       _1,
                                       _2);

        // 'makePredicate' defined above returns a state instead of plain boolean value, so there is
        // no need to use combinator for it.
        generatePredicate(_context,
                          matchExpr->fieldRef(),
                          std::move(makePredicate),
                          LeafTraversalMode::kDoNotTraverseLeaf,
                          false /* useCombinator */);
    }

    void visit(const ElemMatchValueMatchExpression* matchExpr) final {
        using namespace std::placeholders;
        auto numChildren = matchExpr->numChildren();
        invariant(numChildren >= 1);

        tassert(5273406,
                "Eval frame's input slot is not defined",
                _context->evalStack.topFrame().data().inputSlot);
        auto childInputSlot = *_context->evalStack.topFrame().data().inputSlot;

        // Move the children's outputs off of the evalStack into a vector in preparation for
        // calling generateShortCircuitingLogicalOp().
        std::vector<EvalExprStagePair> childStages;
        for (size_t i = 0; i < numChildren; ++i) {
            auto [expr, stage] = _context->evalStack.popFrame();
            childStages.emplace_back(std::move(expr), std::move(stage));
        }
        std::reverse(childStages.begin(), childStages.end());

        auto [filterExpr, filterStage] =
            generateShortCircuitingLogicalOp(sbe::EPrimBinary::logicAnd,
                                             std::move(childStages),
                                             _context->planNodeId,
                                             _context->state.slotIdGenerator,
                                             _context->stateHelper);

        sbe::value::SlotId filterSlot;
        std::tie(filterSlot, filterStage) = projectEvalExpr(std::move(filterExpr),
                                                            std::move(filterStage),
                                                            _context->planNodeId,
                                                            _context->state.slotIdGenerator);

        // We're using 'kDoNotTraverseLeaf' traverse mode, so we're guaranteed that 'makePredcate'
        // will only be called once, so it's safe to bind the reference to 'filterStage' subtree
        // here.
        auto makePredicate = std::bind(&elemMatchMakePredicate,
                                       _context,
                                       filterSlot,
                                       std::ref(filterStage),
                                       childInputSlot,
                                       _1,
                                       _2);

        // 'makePredicate' defined above returns a state instead of plain boolean value, so there is
        // no need to use combinator for it.
        generatePredicate(_context,
                          matchExpr->fieldRef(),
                          std::move(makePredicate),
                          LeafTraversalMode::kDoNotTraverseLeaf,
                          false /* useCombinator */);
    }

    void visit(const EqualityMatchExpression* expr) final {
        generateComparison(_context, expr, sbe::EPrimBinary::eq);
    }

    void visit(const ExistsMatchExpression* expr) final {
        const auto traversalMode = LeafTraversalMode::kDoNotTraverseLeaf;

        auto makePredicate = [expr, context = _context](sbe::value::SlotId inputSlot,
                                                        EvalStage inputStage) -> EvalExprStagePair {
            auto resultExpr = sbe::makeE<sbe::EFunction>(
                "exists", sbe::makeEs(sbe::makeE<sbe::EVariable>(inputSlot)));

            // $exists is always applied to the leaf of the field path. For kDoNotTraverseLeaf mode,
            // generatePredicate() does not convert the predicate value to state when generating
            // traversal for leaf nodes of field path. For this reason, we need to perform this
            // conversion manually.
            if (expr->fieldRef() && !expr->fieldRef()->empty() &&
                context->evalStack.topFrame().data().inputSlot) {
                resultExpr = context->stateHelper.makeState(std::move(resultExpr));
            }

            return {std::move(resultExpr), std::move(inputStage)};
        };

        generatePredicate(_context, expr->fieldRef(), std::move(makePredicate), traversalMode);
    }

    void visit(const ExprMatchExpression* matchExpr) final {
        auto& frame = _context->evalStack.topFrame();

        // The $expr expression must by applied to the current $$ROOT document, so make sure that
        // an input slot associated with the current frame is the same slot as the input slot for
        // the entire match expression we're translating
        tassert(5273407, "Match expression's input slot is not defined", _context->inputSlot);
        tassert(5273408, "Eval frame's input slot is not defined", frame.data().inputSlot);
        tassert(5273409,
                "Eval frame for $expr is not computed over expression's input slot",
                *frame.data().inputSlot == *_context->inputSlot);

        auto currentStage = stageOrLimitCoScan(frame.extractStage(), _context->planNodeId);
        auto&& [expr, stage] = generateExpression(_context->state,
                                                  matchExpr->getExpression().get(),
                                                  std::move(currentStage),
                                                  *frame.data().inputSlot,
                                                  _context->planNodeId);
        auto frameId = _context->state.frameId();

        // We will need to convert the result of $expr to a boolean value, so we'll wrap it into an
        // expression which does exactly that.
        auto logicExpr = generateCoerceToBoolExpression(sbe::EVariable{frameId, 0});

        auto localBindExpr = sbe::makeE<sbe::ELocalBind>(
            frameId, sbe::makeEs(expr.extractExpr()), std::move(logicExpr));

        frame.pushExpr(_context->stateHelper.makeState(std::move(localBindExpr)));
        frame.setStage(std::move(stage));
    }

    void visit(const GTEMatchExpression* expr) final {
        generateComparison(_context, expr, sbe::EPrimBinary::greaterEq);
    }

    void visit(const GTMatchExpression* expr) final {
        generateComparison(_context, expr, sbe::EPrimBinary::greater);
    }

    void visit(const GeoMatchExpression* expr) final {}
    void visit(const GeoNearMatchExpression* expr) final {}

    void visit(const InMatchExpression* expr) final {
        // If there's an "inputParamId" in this expr meaning this expr got parameterized, we can
        // register a SlotId for it and use the slot directly. Note that we don't auto-parameterize
        // $in if it contains null, regexes, or nested arrays.
        if (auto inputParam = expr->getInputParamId()) {
            auto inputParamSlotId =
                _context->state.registerInputParamSlot(*expr->getInputParamId());
            auto makePredicate = [&](sbe::value::SlotId inputSlot,
                                     EvalStage inputStage) -> EvalExprStagePair {
                return {makeIsMember(makeVariable(inputSlot),
                                     makeVariable(inputParamSlotId),
                                     _context->state.data->env),
                        std::move(inputStage)};
            };

            generatePredicate(_context,
                              expr->fieldRef(),
                              std::move(makePredicate),
                              LeafTraversalMode::kArrayElementsOnly);
            return;
        }

        auto&& [arrSetTag, arrSetVal, hasArray, hasNull] = convertInExpressionEqualities(expr);
        sbe::value::ValueGuard arrSetGuard{arrSetTag, arrSetVal};

        const auto traversalMode = hasArray ? LeafTraversalMode::kArrayAndItsElements
                                            : LeafTraversalMode::kArrayElementsOnly;

        // If the InMatchExpression doesn't carry any regex patterns, we can just check if the value
        // in bound to the inputSlot is a member of the equalities set.
        if (expr->getRegexes().size() == 0) {
            auto makePredicate =
                [&, arrSetTag = arrSetTag, arrSetVal = arrSetVal, hasNull = hasNull](
                    sbe::value::SlotId inputSlot, EvalStage inputStage) -> EvalExprStagePair {
                // We have to match nulls and undefined if a 'null' is present in equalities.
                auto inputExpr = !hasNull
                    ? makeVariable(inputSlot)
                    : sbe::makeE<sbe::EIf>(generateNullOrMissing(sbe::EVariable(inputSlot)),
                                           makeConstant(sbe::value::TypeTags::Null, 0),
                                           makeVariable(inputSlot));

                arrSetGuard.reset();
                return {makeIsMember(std::move(inputExpr),
                                     sbe::makeE<sbe::EConstant>(arrSetTag, arrSetVal),
                                     _context->state.data->env),
                        std::move(inputStage)};
            };

            generatePredicate(_context, expr->fieldRef(), std::move(makePredicate), traversalMode);
            return;
        } else {
            // If the InMatchExpression contains regex patterns, then we need to handle a regex-only
            // case and a case where both equalities and regexes are present. The regex-only case is
            // handled by building a traversal stage to traverse the array of regexes and call the
            // 'regexMatch' built-in to check if the field being traversed has a value that matches
            // a regex. We also call 'isMember' to determine whether any of the values are equal
            // to any of the regexes. The combined case uses a short-circuiting limit-1/union OR
            // stage to first exhaust the equalities 'isMember' check, and then if no match is found
            // it executes the regex-only traversal stage.
            auto& regexes = expr->getRegexes();
            auto& equalities = expr->getEqualities();

            auto [pcreArrTag, pcreArrVal] = sbe::value::makeNewArray();
            sbe::value::ValueGuard pcreArrGuard{pcreArrTag, pcreArrVal};
            auto pcreArr = sbe::value::getArrayView(pcreArrVal);

            auto [bsonRegexArrSetTag, bsonRegexArrSetVal] = sbe::value::makeNewArraySet();
            sbe::value::ValueGuard bsonRegexArrSetGuard{bsonRegexArrSetTag, bsonRegexArrSetVal};
            auto bsonRegexArrSet = sbe::value::getArraySetView(bsonRegexArrSetVal);

            if (regexes.size()) {
                pcreArr->reserve(regexes.size());

                for (auto&& r : regexes) {
                    auto [pcreRegexTag, pcreRegexVal] =
                        sbe::value::makeNewPcreRegex(r->getString(), r->getFlags());
                    pcreArr->push_back(pcreRegexTag, pcreRegexVal);

                    auto [bsonRegexTag, bsonRegexVal] =
                        sbe::value::makeNewBsonRegex(r->getString(), r->getFlags());
                    bsonRegexArrSet->push_back(bsonRegexTag, bsonRegexVal);
                }
            }

            auto makePredicate = [&,
                                  arrSetTag = arrSetTag,
                                  arrSetVal = arrSetVal,
                                  pcreRegexArrTag = pcreArrTag,
                                  pcreRegexArrVal = pcreArrVal,
                                  regexArrTag = bsonRegexArrSetTag,
                                  regexArrVal = bsonRegexArrSetVal,
                                  hasNull = hasNull](sbe::value::SlotId inputSlot,
                                                     EvalStage inputStage) -> EvalExprStagePair {
                auto regexArraySlot{_context->state.slotId()};
                auto bsonRegexArraySetSlot{_context->state.slotId()};
                auto regexInputSlot{_context->state.slotId()};
                auto regexOutputSlot{_context->state.slotId()};

                // Build a traverse stage that traverses the query regex pattern array. Here the
                // FROM branch binds an array constant carrying the regex patterns to a slot. Then
                // the inner branch executes 'regexMatch' once per regex.
                auto [regexTag, regexVal] = sbe::value::copyValue(pcreRegexArrTag, pcreRegexArrVal);
                auto [bsonRegexTag, bsonRegexVal] = sbe::value::copyValue(regexArrTag, regexArrVal);

                auto regexFromStage =
                    makeProject(equalities.size() > 0 ? EvalStage{} : std::move(inputStage),
                                _context->planNodeId,
                                regexArraySlot,
                                sbe::makeE<sbe::EConstant>(regexTag, regexVal),
                                bsonRegexArraySetSlot,
                                sbe::makeE<sbe::EConstant>(bsonRegexTag, bsonRegexVal));

                auto regexInnerStage = makeProject(
                    EvalStage{},
                    _context->planNodeId,
                    regexInputSlot,
                    makeBinaryOp(
                        sbe::EPrimBinary::logicOr,
                        makeFillEmptyFalse(sbe::makeE<sbe::EFunction>(
                            "regexMatch",
                            sbe::makeEs(sbe::makeE<sbe::EVariable>(regexArraySlot),
                                        sbe::makeE<sbe::EVariable>(inputSlot)))),
                        makeFillEmptyFalse(sbe::makeE<sbe::EFunction>(
                            "isMember",
                            sbe::makeEs(sbe::makeE<sbe::EVariable>(inputSlot),
                                        sbe::makeE<sbe::EVariable>(bsonRegexArraySetSlot))))));

                auto regexStage =
                    makeTraverse(std::move(regexFromStage),
                                 std::move(regexInnerStage),
                                 regexArraySlot,
                                 regexOutputSlot,
                                 regexInputSlot,
                                 makeBinaryOp(sbe::EPrimBinary::logicOr,
                                              sbe::makeE<sbe::EVariable>(regexOutputSlot),
                                              sbe::makeE<sbe::EVariable>(regexInputSlot)),
                                 sbe::makeE<sbe::EVariable>(regexOutputSlot),
                                 _context->planNodeId,
                                 0);

                // If equalities are present in addition to regexes, build a limit-1/union
                // short-circuiting OR between a filter stage that checks membership of the field
                // being traversed in the equalities and the regex traverse stage
                if (equalities.size() > 0) {
                    std::vector<EvalExprStagePair> branches;

                    // We have to match nulls and undefined if a 'null' is present in equalities.
                    auto inputExpr = !hasNull
                        ? makeVariable(inputSlot)
                        : sbe::makeE<sbe::EIf>(generateNullOrMissing(sbe::EVariable(inputSlot)),
                                               makeConstant(sbe::value::TypeTags::Null, 0),
                                               makeVariable(inputSlot));

                    arrSetGuard.reset();
                    branches.emplace_back(
                        makeIsMember(std::move(inputExpr),
                                     sbe::makeE<sbe::EConstant>(arrSetTag, arrSetVal),
                                     _context->state.data->env),
                        EvalStage{});
                    branches.emplace_back(regexOutputSlot, std::move(regexStage));

                    auto [shortCircuitingExpr, shortCircuitingStage] =
                        generateShortCircuitingLogicalOp(sbe::EPrimBinary::logicOr,
                                                         std::move(branches),
                                                         _context->planNodeId,
                                                         _context->state.slotIdGenerator,
                                                         BooleanStateHelper{});

                    inputStage =
                        makeLoopJoin(std::move(inputStage),  // NOLINT(bugprone-use-after-move)
                                     std::move(shortCircuitingStage),
                                     _context->planNodeId);

                    return {std::move(shortCircuitingExpr), std::move(inputStage)};
                }

                return {regexOutputSlot, std::move(regexStage)};
            };
            generatePredicate(_context, expr->fieldRef(), std::move(makePredicate), traversalMode);
        }
    }
    // The following are no-ops. The internal expr comparison match expression are produced
    // internally by rewriting an $expr expression to an AND($expr, $_internalExpr[OP]), which can
    // later be eliminated by via a conversion into EXACT index bounds, or remains present. In the
    // latter case we can simply ignore it, as the result of AND($expr, $_internalExpr[OP]) is equal
    // to just $expr.
    void visit(const InternalExprEqMatchExpression* expr) final {
        generateAlwaysBoolean(_context, true);
    }
    void visit(const InternalExprGTMatchExpression* expr) final {
        generateAlwaysBoolean(_context, true);
    }
    void visit(const InternalExprGTEMatchExpression* expr) final {
        generateAlwaysBoolean(_context, true);
    }
    void visit(const InternalExprLTMatchExpression* expr) final {
        generateAlwaysBoolean(_context, true);
    }
    void visit(const InternalExprLTEMatchExpression* expr) final {
        generateAlwaysBoolean(_context, true);
    }

    void visit(const InternalBucketGeoWithinMatchExpression* expr) final {}
    void visit(const InternalSchemaAllElemMatchFromIndexMatchExpression* expr) final {}
    void visit(const InternalSchemaAllowedPropertiesMatchExpression* expr) final {}
    void visit(const InternalSchemaBinDataEncryptedTypeExpression* expr) final {}
    void visit(const InternalSchemaBinDataFLE2EncryptedTypeExpression* expr) final {}
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
        generateComparison(_context, expr, sbe::EPrimBinary::lessEq);
    }

    void visit(const LTMatchExpression* expr) final {
        generateComparison(_context, expr, sbe::EPrimBinary::less);
    }

    void visit(const ModMatchExpression* expr) final {
        // The mod function returns the result of the mod operation between the operand and
        // given divisor, so construct an expression to then compare the result of the operation
        // to the given remainder.
        auto makePredicate = [expr, context = _context](sbe::value::SlotId inputSlot,
                                                        EvalStage inputStage) -> EvalExprStagePair {
            auto frameId = context->state.frameId();
            sbe::EVariable dividend{inputSlot};
            sbe::EVariable dividendConvertedToNumberInt64{frameId, 0};
            auto truncatedArgument = sbe::makeE<sbe::ENumericConvert>(
                makeFunction("trunc"_sd, dividend.clone()), sbe::value::TypeTags::NumberInt64);
            tassert(6142202,
                    "Either both divisor and remainer are parameterized or none",
                    (expr->getDivisorInputParamId() && expr->getRemainderInputParamId()) ||
                        (!expr->getDivisorInputParamId() && !expr->getRemainderInputParamId()));
            // If there's related input param ids in this expr, we can register SlotIds for them,
            // and use generated slots directly.
            std::unique_ptr<sbe::EExpression> divisorExpr =
                [&]() -> std::unique_ptr<sbe::EExpression> {
                if (auto divisorParam = expr->getDivisorInputParamId()) {
                    auto divisorSlotId = context->state.registerInputParamSlot(*divisorParam);
                    return makeVariable(divisorSlotId);
                } else {
                    return makeConstant(sbe::value::TypeTags::NumberInt64,
                                        sbe::value::bitcastFrom<int64_t>(expr->getDivisor()));
                }
            }();
            std::unique_ptr<sbe::EExpression> remainderExpr =
                [&]() -> std::unique_ptr<sbe::EExpression> {
                if (auto remainderParam = expr->getRemainderInputParamId()) {
                    auto remainderSlotId = context->state.registerInputParamSlot(*remainderParam);
                    return makeVariable(remainderSlotId);
                } else {
                    return makeConstant(sbe::value::TypeTags::NumberInt64,
                                        sbe::value::bitcastFrom<int64_t>(expr->getRemainder()));
                }
            }();
            auto modExpression = makeBinaryOp(
                sbe::EPrimBinary::logicAnd,
                // Return false if the dividend cannot be represented as a 64 bit integer.
                makeNot(generateNullOrMissing(dividendConvertedToNumberInt64)),
                makeFillEmptyFalse(makeBinaryOp(sbe::EPrimBinary::eq,
                                                makeFunction("mod"_sd,
                                                             dividendConvertedToNumberInt64.clone(),
                                                             std::move(divisorExpr)),
                                                std::move(remainderExpr))));
            return {
                makeBinaryOp(sbe::EPrimBinary::logicAnd,
                             makeNot(makeBinaryOp(sbe::EPrimBinary::logicOr,
                                                  generateNonNumericCheck(dividend),
                                                  makeBinaryOp(sbe::EPrimBinary::logicOr,
                                                               generateNaNCheck(dividend),
                                                               generateInfinityCheck(dividend)))),
                             sbe::makeE<sbe::ELocalBind>(frameId,
                                                         sbe::makeEs(std::move(truncatedArgument)),
                                                         std::move(modExpression))),
                std::move(inputStage)};
        };

        generatePredicate(_context,
                          expr->fieldRef(),
                          std::move(makePredicate),
                          LeafTraversalMode::kArrayElementsOnly);
    }

    void visit(const NorMatchExpression* expr) final {
        // $nor is implemented as a negation of $or. First step is to build $or expression from
        // stack.
        buildLogicalExpression(sbe::EPrimBinary::logicOr, expr->numChildren(), _context);

        // Second step is to negate the result of $or expression.
        // Here we discard the index value of the state even if it was set by expressions below NOR.
        // This matches the behaviour of classic engine, which does not pass 'MatchDetails' object
        // to children of NOR and thus does not get any information on 'elemMatchKey' from them.
        auto& frame = _context->evalStack.topFrame();
        frame.pushExpr(_context->stateHelper.makeState(
            makeNot(_context->stateHelper.getBool(frame.popExpr().extractExpr()))));
    }

    void visit(const NotMatchExpression* expr) final {
        auto& frame = _context->evalStack.topFrame();

        // Negate the result of $not's child.
        // Here we discard the index value of the state even if it was set by expressions below NOT.
        // This matches the behaviour of classic engine, which does not pass 'MatchDetails' object
        // to children of NOT and thus does not get any information on 'elemMatchKey' from them.
        frame.pushExpr(_context->stateHelper.makeState(
            makeNot(_context->stateHelper.getBool(frame.popExpr().extractExpr()))));
    }

    void visit(const OrMatchExpression* expr) final {
        buildLogicalExpression(sbe::EPrimBinary::logicOr, expr->numChildren(), _context);
    }

    void visit(const RegexMatchExpression* expr) final {
        auto makePredicate = [expr, context = _context](sbe::value::SlotId inputSlot,
                                                        EvalStage inputStage) -> EvalExprStagePair {
            tassert(
                6142203,
                "Either both sourceRegex and compiledRegex are parameterized or none",
                (expr->getSourceRegexInputParamId() && expr->getCompiledRegexInputParamId()) ||
                    (!expr->getSourceRegexInputParamId() && !expr->getCompiledRegexInputParamId()));
            std::unique_ptr<sbe::EExpression> bsonRegexExpr =
                [&]() -> std::unique_ptr<sbe::EExpression> {
                if (auto sourceRegexParam = expr->getSourceRegexInputParamId()) {
                    auto sourceRegexSlotId =
                        context->state.registerInputParamSlot(*sourceRegexParam);
                    return makeVariable(sourceRegexSlotId);
                } else {
                    auto [bsonRegexTag, bsonRegexVal] =
                        sbe::value::makeNewBsonRegex(expr->getString(), expr->getFlags());
                    return makeConstant(bsonRegexTag, bsonRegexVal);
                }
            }();

            std::unique_ptr<sbe::EExpression> compiledRegexExpr =
                [&]() -> std::unique_ptr<sbe::EExpression> {
                if (auto compiledRegexParam = expr->getCompiledRegexInputParamId()) {
                    auto compiledRegexSlotId =
                        context->state.registerInputParamSlot(*compiledRegexParam);
                    return makeVariable(compiledRegexSlotId);
                } else {
                    auto [compiledRegexTag, compiledRegexVal] =
                        sbe::value::makeNewPcreRegex(expr->getString(), expr->getFlags());
                    return makeConstant(compiledRegexTag, compiledRegexVal);
                }
            }();

            auto resultExpr = makeBinaryOp(
                sbe::EPrimBinary::logicOr,
                makeFillEmptyFalse(makeBinaryOp(
                    sbe::EPrimBinary::eq, makeVariable(inputSlot), std::move(bsonRegexExpr))),
                makeFillEmptyFalse(makeFunction(
                    "regexMatch", std::move(compiledRegexExpr), makeVariable(inputSlot))));

            return {std::move(resultExpr), std::move(inputStage)};
        };

        generatePredicate(_context,
                          expr->fieldRef(),
                          std::move(makePredicate),
                          LeafTraversalMode::kArrayElementsOnly);
    }

    void visit(const SizeMatchExpression* expr) final {
        generateArraySize(_context, expr);
    }

    void visit(const TextMatchExpression* expr) final {}
    void visit(const TextNoOpMatchExpression* expr) final {}
    void visit(const TwoDPtInAnnulusExpression* expr) final {}

    void visit(const TypeMatchExpression* expr) final {
        // If there's an "inputParamId" in this expr meaning this expr got parameterized, we can
        // register a SlotId for it and use the slot directly. Note that we don't auto-parameterize
        // if the type set contains 'BSONType::Array'.
        if (auto typeMaskParam = expr->getInputParamId()) {
            auto typeMaskSlotId = _context->state.registerInputParamSlot(*typeMaskParam);
            auto makePredicate = [typeMaskSlotId](sbe::value::SlotId inputSlot,
                                                  EvalStage inputStage) -> EvalExprStagePair {
                auto resultExpr = makeFillEmptyFalse(makeFunction(
                    "typeMatch", makeVariable(inputSlot), makeVariable(typeMaskSlotId)));

                return {std::move(resultExpr), std::move(inputStage)};
            };

            generatePredicate(_context,
                              expr->fieldRef(),
                              std::move(makePredicate),
                              LeafTraversalMode::kArrayElementsOnly);

            return;
        }

        const auto traversalMode = expr->typeSet().hasType(BSONType::Array)
            ? LeafTraversalMode::kDoNotTraverseLeaf
            : LeafTraversalMode::kArrayElementsOnly;

        auto makePredicate =
            [expr, traversalMode, context = _context](sbe::value::SlotId inputSlot,
                                                      EvalStage inputStage) -> EvalExprStagePair {
            const MatcherTypeSet& ts = expr->typeSet();
            auto resultExpr = makeFillEmptyFalse(
                makeFunction("typeMatch",
                             makeVariable(inputSlot),
                             makeConstant(sbe::value::TypeTags::NumberInt64,
                                          sbe::value::bitcastFrom<int64_t>(ts.getBSONTypeMask()))));

            // $type is always applied to the leaf of the field path. For kDoNotTraverseLeaf mode,
            // generatePredicate() does not convert the predicate value to state when generating
            // traversal for leaf nodes of field path. For this reason, we need to perform this
            // conversion manually.
            if (expr->fieldRef() && !expr->fieldRef()->empty() &&
                context->evalStack.topFrame().data().inputSlot &&
                traversalMode == LeafTraversalMode::kDoNotTraverseLeaf) {
                resultExpr = context->stateHelper.makeState(std::move(resultExpr));
            }

            return {std::move(resultExpr), std::move(inputStage)};
        };

        generatePredicate(_context, expr->fieldRef(), std::move(makePredicate), traversalMode);
    }

    void visit(const WhereMatchExpression* expr) final {
        auto makePredicate = [expr,
                              ctx = this->_context](sbe::value::SlotId inputSlot,
                                                    EvalStage inputStage) -> EvalExprStagePair {
            // Generally speaking, this visitor is non-destructive and does not mutate the
            // MatchExpression tree. However, in order to apply an optimization to avoid making a
            // copy of the 'JsFunction' object stored within 'WhereMatchExpression', we can transfer
            // its ownership from the match expression node into the SBE plan. Hence, we need to
            // drop the const qualifier. This should be a safe operation, given that the match
            // expression tree is allocated on the heap, and this visitor has exclusive access to
            // this tree (after it has been translated into an SBE tree, it's no longer used).
            auto predicate = makeConstant(
                sbe::value::TypeTags::jsFunction,
                sbe::value::bitcastFrom<JsFunction*>(
                    const_cast<WhereMatchExpression*>(expr)->extractPredicate().release()));

            std::unique_ptr<sbe::EExpression> whereExpr;
            // If there's an "inputParamId" in this expr meaning this expr got parameterized, we can
            // register a SlotId for it and use the slot directly.
            if (auto inputParam = expr->getInputParamId()) {
                auto inputParamSlotId = ctx->state.registerInputParamSlot(*inputParam);
                whereExpr = makeFunction(
                    "runJsPredicate", makeVariable(inputParamSlotId), makeVariable(inputSlot));
            } else {
                whereExpr =
                    makeFunction("runJsPredicate", std::move(predicate), makeVariable(inputSlot));
            }
            return {std::move(whereExpr), std::move(inputStage)};
        };

        generatePredicate(_context,
                          expr->fieldRef(),
                          std::move(makePredicate),
                          LeafTraversalMode::kDoNotTraverseLeaf);
    }

    void visit(const WhereNoOpMatchExpression* expr) final {}
    void visit(const EncryptedBetweenMatchExpression* expr) final {}

private:
    MatchExpressionVisitorContext* _context;
};

/**
 * A match expression in-visitor used for maintaining the counter of the processed child
 * expressions of the nested logical expressions in the match expression tree being traversed.
 */
class MatchExpressionInVisitor final : public MatchExpressionConstVisitor {
public:
    MatchExpressionInVisitor(MatchExpressionVisitorContext* context) : _context(context) {}

    void visit(const AlwaysFalseMatchExpression* expr) final {}
    void visit(const AlwaysTrueMatchExpression* expr) final {}

    void visit(const AndMatchExpression* expr) final {
        if (expr == _context->topLevelAnd) {
            // For a top-level $and, we evaluate each child within the current EvalFrame.
            auto& frame = _context->evalStack.topFrame();
            invariant(frame.exprsCount() > 0);
            frame.setStage(
                makeFilter<false>(frame.extractStage(),
                                  _context->stateHelper.getBool(frame.popExpr().extractExpr()),
                                  _context->planNodeId));
            return;
        }

        // For non-top-level $and's, we evaluate each child in its own EvalFrame, and we
        // leave these EvalFrames on the stack until we're done evaluating all the children.
        pushFrameForLogicalExpressionChild(_context, expr->numChildren());
    }

    void visit(const BitsAllClearMatchExpression* expr) final {}
    void visit(const BitsAllSetMatchExpression* expr) final {}
    void visit(const BitsAnyClearMatchExpression* expr) final {}
    void visit(const BitsAnySetMatchExpression* expr) final {}

    void visit(const ElemMatchObjectMatchExpression* matchExpr) final {
        // ElemMatchObjectMatchExpression is guaranteed to always have exactly 1 child, so we don't
        // need to do anything here.
    }

    void visit(const ElemMatchValueMatchExpression* matchExpr) final {
        const auto& frame = _context->evalStack.topFrame();

        // We leave each child's EvalFrame on the stack until we're finished evaluating all of
        // the children. Set up a new EvalFrame for the next child with a null tree and with the
        // 'inputSlot' field set to 'childInputSlot'. childInputSlot is a "correlated slot" that
        // will be set up later (handled in the post-visitor).
        _context->evalStack.emplaceFrame(EvalStage{}, frame.data().inputSlot);
    }

    void visit(const EqualityMatchExpression* expr) final {}
    void visit(const ExistsMatchExpression* expr) final {}
    void visit(const ExprMatchExpression* expr) final {}
    void visit(const GTEMatchExpression* expr) final {}
    void visit(const GTMatchExpression* expr) final {}
    void visit(const GeoMatchExpression* expr) final {}
    void visit(const GeoNearMatchExpression* expr) final {}
    void visit(const InMatchExpression* expr) final {}
    void visit(const InternalBucketGeoWithinMatchExpression* expr) final {}
    void visit(const InternalExprEqMatchExpression* expr) final {}
    void visit(const InternalExprGTMatchExpression* expr) final {}
    void visit(const InternalExprGTEMatchExpression* expr) final {}
    void visit(const InternalExprLTMatchExpression* expr) final {}
    void visit(const InternalExprLTEMatchExpression* expr) final {}
    void visit(const InternalSchemaAllElemMatchFromIndexMatchExpression* expr) final {}
    void visit(const InternalSchemaAllowedPropertiesMatchExpression* expr) final {}
    void visit(const InternalSchemaBinDataEncryptedTypeExpression* expr) final {}
    void visit(const InternalSchemaBinDataFLE2EncryptedTypeExpression* expr) final {}
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

    void visit(const NorMatchExpression* expr) final {
        // We leave the EvalFrame of each child on the stack until we're done evaluating all the
        // children.
        pushFrameForLogicalExpressionChild(_context, expr->numChildren());
    }

    void visit(const NotMatchExpression* expr) final {}

    void visit(const OrMatchExpression* expr) final {
        // We leave the EvalFrame of each child on the stack until we're done evaluating all the
        // children.
        pushFrameForLogicalExpressionChild(_context, expr->numChildren());
    }

    void visit(const RegexMatchExpression* expr) final {}
    void visit(const SizeMatchExpression* expr) final {}
    void visit(const TextMatchExpression* expr) final {}
    void visit(const TextNoOpMatchExpression* expr) final {}
    void visit(const TwoDPtInAnnulusExpression* expr) final {}
    void visit(const TypeMatchExpression* expr) final {}
    void visit(const WhereMatchExpression* expr) final {}
    void visit(const WhereNoOpMatchExpression* expr) final {}
    void visit(const EncryptedBetweenMatchExpression* expr) final {}

private:
    MatchExpressionVisitorContext* _context;
};
}  // namespace

std::pair<boost::optional<sbe::value::SlotId>, EvalStage> generateFilter(
    StageBuilderState& state,
    const MatchExpression* root,
    EvalStage stage,
    sbe::value::SlotId inputSlot,
    PlanNodeId planNodeId,
    bool trackIndex) {
    // The planner adds an $and expression without the operands if the query was empty. We can bail
    // out early without generating the filter plan stage if this is the case.
    if (root->matchType() == MatchExpression::AND && root->numChildren() == 0) {
        return {boost::none, std::move(stage)};
    }

    // We only use the classic matcher path (aka "franken matcher") when the SBE is not fully
    // enabled. Fully enabling SBE turns on the SBE plan cache, and embedding the classic matcher
    // into the query execution tree is not compatible with the plan cache's use of
    // auto-parameterization. This is because when embedding the classic matcher all of the
    // constants used in the filter are in the MatchExpression itself rather than in slots.
    if (!feature_flags::gFeatureFlagSbeFull.isEnabledAndIgnoreFCV()) {
        tassert(6681403, "trackIndex=true not supported for classic matcher in SBE", !trackIndex);

        auto expr = makeFunction("applyClassicMatcher",
                                 makeConstant(sbe::value::TypeTags::classicMatchExpresion,
                                              sbe::value::bitcastFrom<const MatchExpression*>(
                                                  root->shallowClone().release())),
                                 makeVariable(inputSlot));

        auto filterStage = makeFilter<false>(std::move(stage), std::move(expr), planNodeId);
        return {boost::none, std::move(filterStage)};
    }

    auto stateHelper = makeFilterStateHelper(trackIndex);
    MatchExpressionVisitorContext context{
        state, std::move(stage), inputSlot, root, planNodeId, *stateHelper};
    MatchExpressionPreVisitor preVisitor{&context};
    MatchExpressionInVisitor inVisitor{&context};
    MatchExpressionPostVisitor postVisitor{&context};
    MatchExpressionWalker walker{&preVisitor, &inVisitor, &postVisitor};
    tree_walker::walk<true, MatchExpression>(root, &walker);
    auto [resultSlot, resultStage] = context.done();
    return {resultSlot, std::move(resultStage)};
}

EvalStage generateIndexFilter(StageBuilderState& state,
                              const MatchExpression* root,
                              EvalStage stage,
                              sbe::value::SlotVector keySlots,
                              std::vector<std::string> keyFields,
                              PlanNodeId planNodeId) {
    // The planner adds an $and expression without the operands if the query was empty. We can bail
    // out early without generating the filter plan stage if this is the case.
    if (root->matchType() == MatchExpression::AND && root->numChildren() == 0) {
        return stage;
    }

    // We only use the classic matcher path (aka "franken matcher") when SBE is not fully enabled.
    // Fully enabling SBE turns on the SBE plan cache, and embedding the classic matcher into the
    // query execution tree is not compatible with the plan cache's use of auto-parameterization.
    // This is because when embedding the classic mathcer all of the constants used in the filter
    // are in the MatchExpression itself rather than in slots.
    if (!feature_flags::gFeatureFlagSbeFull.isEnabledAndIgnoreFCV()) {
        BSONObjBuilder keyPatternBuilder;
        for (auto& field : keyFields) {
            keyPatternBuilder.append(field, 1);
        }
        auto keyPatternTree = buildKeyPatternTree(keyPatternBuilder.obj(), keySlots);
        auto mkObjExpr = buildNewObjExpr(keyPatternTree.get());

        auto expr = makeFunction("applyClassicMatcher",
                                 makeConstant(sbe::value::TypeTags::classicMatchExpresion,
                                              sbe::value::bitcastFrom<const MatchExpression*>(
                                                  root->shallowClone().release())),
                                 std::move(mkObjExpr));

        return makeFilter<false>(std::move(stage), std::move(expr), planNodeId);
    }

    // Covered filters never need to track the index of a matching element in the array as they
    // cannot be used with a positional projection.
    const bool trackIndex = false;
    auto stateHelper = makeFilterStateHelper(trackIndex);
    MatchExpressionVisitorContext context{state,
                                          std::move(stage),
                                          std::move(keySlots),
                                          std::move(keyFields),
                                          root,
                                          planNodeId,
                                          *stateHelper};
    MatchExpressionPreVisitor preVisitor{&context};
    MatchExpressionInVisitor inVisitor{&context};
    MatchExpressionPostVisitor postVisitor{&context};
    MatchExpressionWalker walker{&preVisitor, &inVisitor, &postVisitor};
    tree_walker::walk<true, MatchExpression>(root, &walker);

    auto [resultSlot, resultStage] = context.done();
    tassert(5273411, "Index filter must not track a matching element index", !resultSlot);
    return std::move(resultStage);
}

std::tuple<sbe::value::TypeTags, sbe::value::Value, bool, bool> convertInExpressionEqualities(
    const InMatchExpression* expr) {
    auto& equalities = expr->getEqualities();
    auto [arrSetTag, arrSetVal] = sbe::value::makeNewArraySet();
    sbe::value::ValueGuard arrSetGuard{arrSetTag, arrSetVal};

    auto arrSet = sbe::value::getArraySetView(arrSetVal);

    auto hasArray = false;
    auto hasNull = false;
    if (equalities.size()) {
        arrSet->reserve(equalities.size());
        for (auto&& equality : equalities) {
            auto [tagView, valView] =
                sbe::bson::convertFrom<true>(equality.rawdata(),
                                             equality.rawdata() + equality.size(),
                                             equality.fieldNameSize() - 1);

            hasNull |= tagView == sbe::value::TypeTags::Null;
            hasArray |= sbe::value::isArray(tagView);

            // An ArraySet assumes ownership of it's values so we have to make a copy here.
            auto [tag, val] = sbe::value::copyValue(tagView, valView);
            arrSet->push_back(tag, val);
        }
    }

    arrSetGuard.reset();
    return {arrSetTag, arrSetVal, hasArray, hasNull};
}

std::pair<sbe::value::TypeTags, sbe::value::Value> convertBitTestBitPositions(
    const BitTestMatchExpression* expr) {
    auto bitPositions = expr->getBitPositions();

    // Build an array set of bit positions for the bitmask, and remove duplicates in the
    // bitPositions vector since duplicates aren't handled in the match expression parser by
    // checking if an item has already been seen.
    auto [bitPosTag, bitPosVal] = sbe::value::makeNewArray();
    sbe::value::ValueGuard arrGuard{bitPosTag, bitPosVal};

    auto arr = sbe::value::getArrayView(bitPosVal);
    if (bitPositions.size()) {
        arr->reserve(bitPositions.size());

        std::set<uint32_t> seenBits;
        for (size_t index = 0; index < bitPositions.size(); ++index) {
            auto currentBit = bitPositions[index];
            if (auto result = seenBits.insert(currentBit); result.second) {
                arr->push_back(sbe::value::TypeTags::NumberInt64,
                               sbe::value::bitcastFrom<int64_t>(currentBit));
            }
        }
    }

    arrGuard.reset();
    return {bitPosTag, bitPosVal};
}

}  // namespace mongo::stage_builder
