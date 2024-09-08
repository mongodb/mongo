/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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


#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_set.h>
#include <absl/container/inlined_vector.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/ordering.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_catalog_entry.h"
#include "mongo/db/curop.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/expressions/runtime_environment.h"
#include "mongo/db/exec/sbe/stages/branch.h"
#include "mongo/db/exec/sbe/stages/filter.h"
#include "mongo/db/exec/sbe/stages/hash_agg.h"
#include "mongo/db/exec/sbe/stages/hash_lookup.h"
#include "mongo/db/exec/sbe/stages/hash_lookup_unwind.h"
#include "mongo/db/exec/sbe/stages/ix_scan.h"
#include "mongo/db/exec/sbe/stages/limit_skip.h"
#include "mongo/db/exec/sbe/stages/loop_join.h"
#include "mongo/db/exec/sbe/stages/makeobj.h"
#include "mongo/db/exec/sbe/stages/project.h"
#include "mongo/db/exec/sbe/stages/scan.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/stages/union.h"
#include "mongo/db/exec/sbe/stages/unique.h"
#include "mongo/db/exec/sbe/stages/unwind.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index_names.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/query/bson_typemask.h"
#include "mongo/db/query/index_entry.h"
#include "mongo/db/query/index_hint.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_yield_policy_sbe.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/db/query/stage_builder/sbe/builder.h"
#include "mongo/db/query/stage_builder/sbe/gen_helpers.h"
#include "mongo/db/query/stage_builder/sbe/gen_projection.h"
#include "mongo/db/query/stage_builder/sbe/sbexpr_helpers.h"
#include "mongo/db/query/stage_types.h"
#include "mongo/db/query/util/make_data_structure.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::stage_builder {

/**
 * Helpers for building $lookup.
 */
namespace {
/**
 * De-facto MQL semantics for matching local or foreign records to 'null' is complex and therefore
 * is causing complex SBE trees to implement it. This comment describes the semantics.
 *
 * =================================================================================================
 * Definitions:
 *  1. "path" or "path spec" - the sequence of field names to access nested objects
 *  2. "resolved path" - the sequence of objects a path has been resolved to inside a particular
 *      object. If some of the fields are arrays, there might be multiple resolved paths that
 *      correspond to the same path spec. We'll denote a resolved path with a string that encodes
 *      which elements of the arrays have been accessed. For example, for a path "a.b.c" resolved
 *      paths might look like "a.b.c", "a.b._", "a.0.b.0.c" or "a.0.b.1._" (underscore shows that
 *      couldn't continue resolving the path).
 *  3. "terminal" - the value at the end of a resolved path, if the resolved path terminates before
 *      fully implementing a path spec, we'll call the terminal "missing".
 *
 * For example, given object {a: [{b: [{c: [1,2]}, {no_c: 3}]}, {b: {c: 4}}, {no_b: 5}]} and path
 * spec 'a.b.c', there are four resolved paths:
 *  - a.0.b.0.c - terminal: [1,2]
 *  - a.0.b.1._ - terminal: missing
 *  - a.1.b.c   - terminal: 4
 *  - a.2._     - terminal: missing
 *
 * =================================================================================================
 * Matching local records to null
 *
 * Foreign record {key: null}, assuming foreignField:'key', would match to local records that meet
 * the following conditions when traversing localField path:
 *
 * 1. there is a terminal with value 'null' or a value of array that contains 'null'. For example,
 *    if localField:"a.b", the following records would match:
 *      {a: {b: null}}
 *      {a: {b: [1, null, 2]}}
 *      {a: [{b: [1, null, 2]}, {b: 3}]}
 *
 * 2. all terminals are either missing or have value of an empty array. For example, if
 *    localField:'a.b', the following records would match:
 *      {a: {b: []}}
 *      {a: {no_b: 1}}
 *      {a: [{b: []}, {no_b: 1}]}
 *      {a: [1, 2]}
 *      {no_a: 1}
 *
 * =================================================================================================
 * Matching foreign records to null (same as in the 'find' sub-system)
 *
 * Local record {key: null}, assuming localField:'key', would match to foreign records that meet the
 * following conditions when traversing foreignField path:

 * 1. (same as when matching local) there is a terminal with value 'null' or a value of array that
 *    contains 'null'. For example, if foreignField:'a.b', the following records would match:
 *      {a: {b: null}}
 *      {a: {b: [1, null, 2]}}
 *      {a: [{b: [1, null, 2]}, {b: 3}]}
 *
 * 2. there is a missing terminal, such that the last value on the resolved path to this terminal
 *    is not a scalar inside array. For example, if foreignField:"a.b.c", the following records
 *    would match:
 *      {a: {b: {no_c: 1}}} // a.b._ last value {no_c: 1} isn't a scalar
 *      {a: {b: 1}} // a.b._ last value 1 is a scalar but it's not inside an array
 *      {a: [{b: {no_c: 1}}, {b: {c: 2}}]} // a.0.b._ last value {no_c: 1} isn't a scalar
 *      {a: [{b: [{c: 1}, {c: 2}]}, {b: [{c: 3}, {no_c: 4}]}]} // a.1.b.1._ last value {no_c: 4}
 *      {a: 1} // a._ last value 1 is a scalar but it's not inside an array
 *      {no_a: 1} // _ last value {no_a: 1} isn't a scalar
 *
 *    but these records won't match:
 *      {a: [1, 2]} // a.0._ and a.1._ end in scalar values inside array
 *      {a: {b: [1, 2]}} // a.b.0._ and a.b.1._ end in scalar values inside array
 *      {a: [{b: [1, 2]}, 3]} // a.0.b.0._, a.0.b.1._ and a.1.b._ end in scalar values inside arrays
 */

enum class JoinSide { Local = 0, Foreign = 1 };

// Creates an expression for traversing path 'fp' in the record from 'inputSlot' that implement MQL
// semantics for local collections. The semantics never treat terminal arrays as whole values and
// match to null per "Matching local records to null" above. Returns all the key values in a single
// array. For example, if the record in the 'inputSlot' is:
//     {a: [{b:[1,[2,3]]}, {b:4}, {b:1}, {b:2}]},
// the returned values for path "a.b" will be packed as: [1, [2,3], 4, 1, 2].
// Empty arrays and missing are skipped, that is, if the record in the 'inputSlot' is:
//     {a: [{b:1}, {b:[]}, {no_b:42}, {b:2}]},
// the returned values for path "a.b" will be packed as: [1, 2].
SbExpr generateLocalKeyStream(SbExpr inputExpr,
                              const FieldPath& fp,
                              size_t level,
                              StageBuilderState& state,
                              boost::optional<SbSlot> topLevelFieldSlot = boost::none) {
    using namespace std::literals;

    SbExprBuilder b(state);
    invariant(level < fp.getPathLength());

    tassert(9033500,
            "Expected an input expression or top level field",
            !inputExpr.isNull() || topLevelFieldSlot.has_value());

    // Generate an expression to read a sub-field at the current nested level.
    SbExpr fieldName = b.makeStrConstant(fp.getFieldName(level));
    SbExpr fieldExpr = topLevelFieldSlot
        ? SbExpr{*topLevelFieldSlot}
        : b.makeFunction("getField"_sd, std::move(inputExpr), std::move(fieldName));

    if (level == fp.getPathLength() - 1) {
        // The last level doesn't expand leaf arrays.
        return fieldExpr;
    }

    // Generate nested traversal.
    sbe::FrameId lambdaFrameId = state.frameId();

    SbExpr resultExpr = generateLocalKeyStream(SbLocalVar{lambdaFrameId, 0}, fp, level + 1, state);

    SbExpr lambdaExpr = b.makeLocalLambda(lambdaFrameId, std::move(resultExpr));

    // Generate the traverse stage for the current nested level. If the traversed field is an array,
    // we know that traverseP will wrap the result into an array, that we need to remove by using
    // unwindArray.
    // For example, when processing
    //     {a: [{b:[1,[2,3]]}, {b:4}, {b:1}, {b:2}]}
    // the result of getField("a") is an array, and traverseP will return an array
    //     [ [1,[2,3]], 4, 1, 2 ]
    // holding the results of the lambda for each item; in order to obtain the list of leaf nodes we
    // have to extract the content of the first item into the containing array, e.g.
    //     [ 1, [2,3], 4, 1, 2 ]
    // When traverseP processes a non-array, the result could still be an array, but it would be the
    // result of running the lambda on a non-array value, e.g.
    //     {a: {b:[1, [2]]} }
    // The result would be [1, [2]] that is already in the correct form and should not be processed
    // with unwindArray, or the result would be an incorrect [1, 2].
    sbe::FrameId traverseFrameId = state.frameId();
    return b.makeLet(traverseFrameId,
                     makeVector(std::move(fieldExpr),
                                b.makeFunction("traverseP"_sd,
                                               SbLocalVar{traverseFrameId, 0},
                                               std::move(lambdaExpr),
                                               b.makeInt32Constant(1))),
                     b.makeIf(b.makeFunction("isArray"_sd, SbLocalVar{traverseFrameId, 0}),
                              b.makeFunction("unwindArray"_sd, SbLocalVar{traverseFrameId, 1}),
                              SbLocalVar{traverseFrameId, 1}));
}

// Creates stages for traversing path 'fp' in the record from 'inputSlot' that implement MQL
// semantics for foreign collections. Returns one key value at a time, including terminal arrays as
// a whole value. For example,
// if the record in the 'inputSlot' is:
//     {a: [{b:[1,[2,3]]}, {b:4}, {b:1}, {b:2}]},
// the returned values for path "a.b" will be streamed as: 1, [2,3], [1, [2, 3]], 4, 1, 2.
// Scalars inside arrays on the path are skipped, that is, if the record in the 'inputSlot' is:
//     {a: [42, {b:{c:1}}, {b: [41,42,{c:2}]}, {b:42}, {b:{c:3}}]},
// the returned values for path "a.b.c" will be streamed as: 1, 2, null, 3.
// Replaces other missing terminals with 'null', that is, if the record in the 'inputSlot' is:
//     {a: [{b:1}, {b:[]}, {no_b:42}, {b:2}]},
// the returned values for path "a.b" will be streamed as: 1, [], null, 2.
std::pair<SbSlot /* keyValueSlot */, SbStage> buildForeignKeysStream(SbSlot inputSlot,
                                                                     const FieldPath& fp,
                                                                     const PlanNodeId nodeId,
                                                                     StageBuilderState& state) {
    SbBuilder b(state, nodeId);

    const FieldIndex numParts = fp.getPathLength();

    SbSlot keyValueSlot = inputSlot;
    SbSlot prevKeyValueSlot = inputSlot;
    SbStage currentStage = b.makeLimitOneCoScanTree();

    for (size_t i = 0; i < numParts; i++) {
        const StringData fieldName = fp.getFieldName(i);

        SbExpr getFieldFromObject;
        if (i == 0) {
            // 'inputSlot' must contain a document and, by definition, it's not inside an array, so
            // can get field unconditionally.
            getFieldFromObject = b.makeFillEmptyNull(
                b.makeFunction("getField"_sd, keyValueSlot, b.makeStrConstant(fieldName)));
        } else {
            // Don't get field from scalars inside arrays (it would fail but we also don't want to
            // fill with "null" in this case to match the MQL semantics described above.)
            SbExpr shouldGetField =
                b.makeBinaryOp(sbe::EPrimBinary::logicOr,
                               b.makeFunction("isObject", keyValueSlot),
                               b.makeUnaryOp(sbe::EPrimUnary::logicNot,
                                             b.makeFunction("isArray", prevKeyValueSlot)));

            getFieldFromObject =
                b.makeIf(std::move(shouldGetField),
                         b.makeFillEmptyNull(b.makeFunction(
                             "getField"_sd, keyValueSlot, b.makeStrConstant(fieldName))),
                         b.makeNothingConstant());
        }

        auto [outStage, outSlots] =
            b.makeProject(std::move(currentStage), std::move(getFieldFromObject));
        currentStage = std::move(outStage);
        SbSlot getFieldSlot = outSlots[0];

        keyValueSlot = getFieldSlot;

        // For the terminal array we will do the extra work of adding the array itself to the stream
        // (see below) but for the non-termial path components we only need to unwind array
        // elements.
        if (i + 1 < numParts) {
            constexpr bool preserveNullAndEmptyArrays = true;

            auto [outStage, unwindOutputSlot, _] =
                b.makeUnwind(std::move(currentStage), keyValueSlot, preserveNullAndEmptyArrays);
            currentStage = std::move(outStage);

            prevKeyValueSlot = keyValueSlot;
            keyValueSlot = unwindOutputSlot;
        }
    }

    // For the terminal field part, both the array elements and the array itself are considered as
    // keys. To implement this, we use a "union" stage, where the first branch produces array
    // elements and the second branch produces the array itself. To avoid re-traversing the path, we
    // pass the already traversed path to the "union" via "nlj" stage. However, for scalars 'unwind'
    // produces the scalar itself and we don't want to add it to the stream twice -- this is handled
    // by the 'branch' stage.
    // For example, for foreignField = "a.b" this part of the tree would look like:
    // [2] nlj [] [s17]
    //     left
    //         # Get the terminal value on the path, it will be placed in s17, it might be a scalar
    //         # or it might be an array.
    //         [2] project [s17 = if (
    //               isObject (s15) || ! isArray (s14), fillEmpty (getField (s15, "b"), null),
    //               Nothing)]
    //         [2] unwind s15 s16 s14 true
    //         [2] project [s14 = fillEmpty (getField (s7 = inputSlot, "a"), null)]
    //         [2] limit 1
    //         [2] coscan
    //     right
    //         # Process the terminal value depending on whether it's an array or a scalar/object.
    //         [2] branch {isArray (s17)} [s21]
    //           # If s17 is an array, unwind it and union with the value of the array itself.
    //           [s20] [2] union [s20] [
    //                 [s18] [2] unwind s18 s19 s17 true
    //                       [2] limit 1
    //                       [2] coscan ,
    //                 [s17] [2] limit 1
    //                       [2] coscan
    //                 ]
    //           # If s17 isn't an array, don't need to do anything and simply return s17.
    //           [s17] [2] limit 1
    //                 [2] coscan
    constexpr bool preserveNullAndEmptyArrays = true;

    auto [terminalUnwind, terminalUnwindOutputSlot, _] =
        b.makeUnwind(b.makeLimitOneCoScanTree(), keyValueSlot, preserveNullAndEmptyArrays);

    sbe::PlanStage::Vector terminalStagesToUnion;
    terminalStagesToUnion.push_back(std::move(terminalUnwind));
    terminalStagesToUnion.emplace_back(b.makeLimitOneCoScanTree());

    auto [unionStage, unionOutputSlots] = b.makeUnion(
        std::move(terminalStagesToUnion),
        std::vector{SbExpr::makeSV(terminalUnwindOutputSlot), SbExpr::makeSV(keyValueSlot)});

    auto unionOutputSlot = unionOutputSlots[0];

    auto maybeUnionOutputSlot = SbSlot{state.slotId()};
    unionStage =
        sbe::makeS<sbe::BranchStage>(std::move(unionStage),
                                     b.makeLimitOneCoScanTree(),
                                     b.makeFunction("isArray", keyValueSlot).extractExpr(state),
                                     sbe::makeSV(unionOutputSlot.getId()),
                                     sbe::makeSV(keyValueSlot.getId()),
                                     sbe::makeSV(maybeUnionOutputSlot.getId()),
                                     nodeId);

    currentStage = b.makeLoopJoin(std::move(currentStage),
                                  std::move(unionStage),
                                  {} /* outerProjects */,
                                  SbExpr::makeSV(keyValueSlot) /* outerCorrelated */);
    keyValueSlot = maybeUnionOutputSlot;

    return {keyValueSlot, std::move(currentStage)};
}

// Returns the vector of local slots to be used in lookup join, including the record slot and
// metadata slots produced by local side.
SbSlotVector buildLocalSlots(StageBuilderState& state, SbSlot localRecordSlot) {
    auto metadataSlotIds = state.data->metadataSlots.getSlotVector();

    SbSlotVector slots;
    slots.reserve(metadataSlotIds.size() + 1);

    for (auto slotId : metadataSlotIds) {
        slots.push_back(SbSlot{slotId});
    }

    slots.push_back(localRecordSlot);

    return slots;
}

// Creates stages for traversing path 'fp' in the record from 'inputSlot'. Puts the set of key
// values into 'keyValuesSetSlot. For example, if the record in the 'inputSlot' is:
//     {a: [{b:[1,[2,3]]}, {b:4}, {b:1}, {b:2}]},
// the returned slot will contain for path "a.b" a set of {1, 2, 4, [2,3]}.
// If the stream produces no values, that is, would result in an empty set, the empty set is
// replaced with a set that contains a single 'null' value, so that it matches MQL semantics when
// empty arrays and all missing are matched to 'null'.
std::pair<SbSlot /* keyValuesSetSlot */, SbStage> buildKeySet(
    StageBuilderState& state,
    JoinSide joinSide,
    SbStage inputStage,
    SbSlot recordSlot,
    const FieldPath& fp,
    boost::optional<sbe::value::SlotId> collatorSlot,
    const PlanNodeId nodeId) {
    SbBuilder b(state, nodeId);

    if (joinSide == JoinSide::Local) {
        auto [arrayWithNullTag, arrayWithNullVal] = sbe::value::makeNewArray();
        sbe::value::Array* arrayWithNullView = sbe::value::getArrayView(arrayWithNullVal);
        arrayWithNullView->push_back(sbe::value::TypeTags::Null, 0);

        sbe::FrameId frameId = state.frameId();
        // The array returned by the expression generated by generateLocalKeyStream might end up
        // empty if the localField contained only missing and empty arrays (e.g. path "a.b" in {a:
        // [{no_b:1}, {b:[]}]}). The semantics of MQL for local keys require these cases to match to
        // 'null', so we replace the empty array with a constant array that contains a single 'null'
        // value.
        SbExpr expr = b.makeLet(
            frameId,
            makeVector(generateLocalKeyStream(SbExpr{recordSlot}, fp, 0, state)),
            b.makeIf(b.makeFillEmptyFalse(b.makeFunction("isArray"_sd, SbLocalVar{frameId, 0})),
                     b.makeIf(b.makeFunction("isArrayEmpty"_sd, SbLocalVar{frameId, 0}),
                              b.makeConstant(arrayWithNullTag, arrayWithNullVal),
                              SbLocalVar{frameId, 0}),
                     b.makeFunction("newArray"_sd, b.makeFillEmptyNull(SbLocalVar{frameId, 0}))));
        // Convert the array into an ArraySet that has no duplicate keys.
        if (collatorSlot) {
            expr = b.makeFunction("collArrayToSet"_sd, SbSlot{*collatorSlot}, std::move(expr));
        } else {
            expr = b.makeFunction("arrayToSet"_sd, std::move(expr));
        }

        auto [outStage, outSlots] = b.makeProject(std::move(inputStage), std::move(expr));
        inputStage = std::move(outStage);

        auto keyValueSlot = outSlots[0];

        return {keyValueSlot, std::move(inputStage)};
    }
    // Create the branch to stream individual key values from every terminal of the path.
    auto [keyValueSlot, keyValuesStage] = buildForeignKeysStream(recordSlot, fp, nodeId, state);

    // Re-pack the individual key values into a set. We don't cap "addToSet" here because its
    // size is bounded by the size of the record.
    auto spillSlot = SbSlot{state.slotId()};

    auto addToSetExpr = collatorSlot
        ? b.makeFunction("collAddToSet"_sd, SbSlot{*collatorSlot}, keyValueSlot)
        : b.makeFunction("addToSet"_sd, keyValueSlot);

    auto aggSetUnionExpr = collatorSlot
        ? b.makeFunction("aggCollSetUnion"_sd, SbSlot{*collatorSlot}, spillSlot)
        : b.makeFunction("aggSetUnion"_sd, spillSlot);

    SbAggExprVector sbAggExprs;
    sbAggExprs.emplace_back(
        SbAggExpr{SbExpr{} /* init */, SbExpr{} /* blockAgg */, std::move(addToSetExpr)},
        boost::none);

    SbExprSbSlotVector mergingExprs;
    mergingExprs.emplace_back(std::move(aggSetUnionExpr), spillSlot);

    auto [packedKeyValuesStage, _, aggOutSlots] =
        b.makeHashAgg(std::move(keyValuesStage),
                      {}, /* groupBy slots - an empty vector means creating a single group */
                      std::move(sbAggExprs),
                      {}, /* We group _all_ key values to a single set so we can ignore collation */
                      std::move(mergingExprs));

    SbSlot keyValuesSetSlot = aggOutSlots[0];

    // Attach the set of key values to the original local record.
    auto nljLocalWithKeyValuesSet =
        b.makeLoopJoin(std::move(inputStage),
                       std::move(packedKeyValuesStage),  // NOLINT(bugprone-use-after-move)
                       SbExpr::makeSV(recordSlot),
                       SbExpr::makeSV(recordSlot) /* outerCorrelated */);

    return {keyValuesSetSlot, std::move(nljLocalWithKeyValuesSet)};
}

// Creates stages for grouping matched foreign records into an array. If there's no match, the
// stages return an empty array instead.
std::pair<SbSlot /* resultSlot */, SbStage> buildForeignMatchedArray(SbStage innerBranch,
                                                                     SbSlot foreignRecordSlot,
                                                                     const PlanNodeId nodeId,
                                                                     StageBuilderState& state) {
    SbBuilder b(state, nodeId);

    // $lookup's aggregates the matching records into an array. We currently don't have a stage
    // that could do this grouping _after_ Nlj, so we achieve it by having a hash_agg inside the
    // inner branch that aggregates all matched records into a single accumulator. When there
    // are no matches, return an empty array.
    const int sizeCap = internalLookupStageIntermediateDocumentMaxSizeBytes.load();
    auto spillSlot = SbSlot{state.slotId()};

    auto addToArrayExpr =
        b.makeFunction("addToArrayCapped"_sd, foreignRecordSlot, b.makeInt32Constant(sizeCap));
    SbAggExprVector sbAggExprs;
    sbAggExprs.emplace_back(
        SbAggExpr{SbExpr{} /* init */, SbExpr{} /* blockAgg */, std::move(addToArrayExpr)},
        boost::none);

    SbExprSbSlotVector mergingExprs;
    mergingExprs.emplace_back(
        b.makeFunction("aggConcatArraysCapped", spillSlot, b.makeInt32Constant(sizeCap)),
        spillSlot);

    auto [hashAggStage, _, aggOutSlots] = b.makeHashAgg(std::move(innerBranch),
                                                        {}, /* groupBy slots */
                                                        std::move(sbAggExprs),
                                                        {}, /* collatorSlot */
                                                        std::move(mergingExprs));
    innerBranch = std::move(hashAggStage);
    SbSlot accumulatorSlot = aggOutSlots[0];

    // 'accumulatorSlot' is either Nothing or contains an array of size two, where the front element
    // is the array of matched records and the back element is their cumulative size (in bytes).
    auto [projectStage, projectOutSlots] = b.makeProject(
        std::move(innerBranch),
        b.makeFunction("getElement",
                       accumulatorSlot,
                       b.makeInt32Constant(static_cast<int>(sbe::vm::AggArrayWithSize::kValues))));
    innerBranch = std::move(projectStage);
    SbSlot matchedRecordsSlot = projectOutSlots[0];

    // $lookup is an _outer_ left join that returns an empty array for "as" field rather than
    // dropping the unmatched local records. The branch that accumulates the matched records into an
    // array returns either 1 or 0 results, so to return an empty array for no-matches case we union
    // this branch with a const scan that produces an empty array but limit it to 1, so if the given
    // branch does produce a record, only that record is returned.
    auto [emptyArrayTag, emptyArrayVal] = sbe::value::makeNewArray();
    // Immediately take ownership of the new array (we could use a ValueGuard here but we'll
    // need the constant below anyway).
    SbExpr emptyArrayConst = b.makeConstant(emptyArrayTag, emptyArrayVal);

    auto [emptyArrayStage, emptyArrayOutSlots] =
        b.makeProject(b.makeLimitOneCoScanTree(), std::move(emptyArrayConst));
    SbSlot emptyArraySlot = emptyArrayOutSlots[0];

    auto unionInputs =
        makeVector(SbExpr::makeSV(matchedRecordsSlot), SbExpr::makeSV(emptyArraySlot));

    auto [unionStage, unionOutputSlots] = b.makeUnion(
        sbe::makeSs(std::move(innerBranch), std::move(emptyArrayStage)), std::move(unionInputs));

    auto unionOutputSlot = unionOutputSlots[0];

    auto limitStage = b.makeLimit(std::move(unionStage), b.makeInt64Constant(1));

    return {unionOutputSlot, std::move(limitStage)};
}  // buildForeignMatchedArray

/**
 * Build keys set for NLJ foreign side using traverseF expression. Creates stages that extract key
 * values from the given foreign record, compares them to the local key values and:
 *   - if 'hasUnwindSrc', acts like a SQL join ($LU aka $lookup-$unwind pattern)
 *   - else groups the matching records into an array (normal MQL $lookup)
 *
 * The traverseF expression will iterate each key value, including terminal arrays as
 * a whole value, and compare it against local key set 'localKeySlot'. For example,
 * if the record in the 'foreignRecordSlot' is:
 *     {a: [{b:[1,[2,3]]}, {b:4}, {b:1}, {b:2}]},
 * path "a.b" will be iterated as: 1, [2,3], [1, [2, 3]], 4, 1, 2.
 * Scalars inside arrays on the path are skipped, that is, if the record in the 'foreignRecordSlot'
 * is:  {a: [42, {b:{c:1}}, {b: [41,42,{c:2}]}, {b:42}, {b:{c:3}}]},
 * path "a.b.c" will be iterated as: 1, 2, null, 3.
 * Replaces other missing terminals with 'null', that is, if the record in the 'foreignRecordSlot'
 * is:  {a: [{b:1}, {b:[]}, {no_b:42}, {b:2}]},
 * path "a.b" will be iterated as: 1, [], null, 2.
 *
 * Here is an example plan for the NLJ inner side:
 * limit 1
 * union [unionOutputSlot] [
 *   branch0[projOutputSlot]
 *     project [projOutputSlot = getElement(groupSlot, 0)]
 *     group [] [groupSlot = addToArrayCapped(foreignRecordSlot, 104857600)]
 *     filter {traverseF (
 *       let [
 *           l11.0 = fillEmpty (getField (foreignRecordSlot, "a"), null)
 *       ]
 *       in
 *           if typeMatch (l11.0, 24)
 *           then l11.0
 *           else Nothing
 *       , lambda(l3.0) {
 *           if fillEmpty (isObject (l3.0), true)
 *           then traverseF (
 *             fillEmpty (getField (l3.0, "b"), null), lambda(l2.0) {isMember (l2.0, localKeySlot)},
 *             true),
 *           else false
 *        }, false)}
 *     scan foreignRecordSlot recordIdSlot none none none none [] @uuid true false
 * branch1[emptySlot] project [emptySlot = []] limit 1 coscan
 * ]
 */
std::pair<SbSlot /* matched docs */, SbStage> buildForeignMatches(SbSlot localKeySlot,
                                                                  SbStage foreignStage,
                                                                  SbSlot foreignRecordSlot,
                                                                  const FieldPath& foreignFieldName,
                                                                  const PlanNodeId nodeId,
                                                                  StageBuilderState& state,
                                                                  bool hasUnwindSrc) {
    SbBuilder b(state, nodeId);

    auto frameId = state.frameId();

    auto lambdaArg = SbExpr{SbVar{frameId, 0}};
    auto filter = b.makeFunction("isMember"_sd, lambdaArg.clone(), localKeySlot);

    // Recursively create traverseF expressions to iterate elements in 'foreignRecordSlot' with path
    // 'foreignFieldName', and check if key is in set 'localKeySlot'.
    //
    // If a non-terminal field is an array, we will ignore any element that is not an object inside
    // the array.
    const int32_t foreignPathLength = foreignFieldName.getPathLength();
    for (int32_t i = foreignPathLength - 1; i >= 0; --i) {
        auto arrayLambda = b.makeLocalLambda(frameId, std::move(filter));

        frameId = state.frameId();
        lambdaArg = i == 0 ? SbExpr{foreignRecordSlot} : SbExpr{SbVar{frameId, 0}};

        auto getFieldOrNull = b.makeFillEmptyNull(b.makeFunction(
            "getField"_sd, lambdaArg.clone(), b.makeStrConstant(foreignFieldName.getFieldName(i))));

        // Non object/array field will be converted into Nothing, passing along recursive traverseF
        // and will be treated as null to compared against local key set.
        if (i != foreignPathLength - 1) {
            auto localBindFrameId = state.frameId();

            auto binds = SbExpr::makeSeq(std::move(getFieldOrNull));
            SbVar var = SbVar{localBindFrameId, 0};

            auto innerExpr =
                b.makeIf(b.makeFunction("typeMatch"_sd,
                                        var,
                                        b.makeInt32Constant(getBSONTypeMask(BSONType::Array) |
                                                            getBSONTypeMask(BSONType::Object))),
                         var,
                         b.makeNothingConstant());

            getFieldOrNull = b.makeLet(localBindFrameId, std::move(binds), std::move(innerExpr));
        }

        filter = b.makeFunction("traverseF"_sd,
                                std::move(getFieldOrNull),
                                std::move(arrayLambda),
                                b.makeBoolConstant(i == foreignPathLength - 1) /*compareArray*/);

        if (i > 0) {
            // Ignoring the nulls produced by missing field in array.
            filter = b.makeIf(b.makeBinaryOp(sbe::EPrimBinary::fillEmpty,
                                             b.makeFunction("isObject"_sd, lambdaArg.clone()),
                                             b.makeBoolConstant(true)),
                              std::move(filter),
                              b.makeBoolConstant(false));
        }
    }

    SbStage foreignOutputStage = b.makeFilter(std::move(foreignStage), std::move(filter));
    if (hasUnwindSrc) {
        // $LU [$lookup, $unwind] pattern: The query immediately unwinds the lookup result array. We
        // implement this efficiently by returning a record for each individual foreign match one by
        // one, like a SQL join, instead of accumulating them into an array and then unwinding it.
        return {foreignRecordSlot, std::move(foreignOutputStage)};
    } else {
        // Plain $lookup: Assemble the matched foreign documents into an array. This creates a union
        // stage internally so that when there are no matching foreign records, an empty array will
        // be returned.
        return buildForeignMatchedArray(
            std::move(foreignOutputStage), foreignRecordSlot, nodeId, state);
    }
}  // buildForeignMatches

std::pair<SbSlot /* matched docs */, SbStage> buildNljLookupStage(
    StageBuilderState& state,
    SbStage localStage,
    SbSlot localRecordSlot,
    const FieldPath& localFieldName,
    SbStage foreignStage,
    SbSlot foreignRecordSlot,
    const FieldPath& foreignFieldName,
    boost::optional<sbe::value::SlotId> collatorSlot,
    const PlanNodeId nodeId,
    bool hasUnwindSrc) {
    SbBuilder b(state, nodeId);

    CurOp::get(state.opCtx)->debug().nestedLoopJoin += 1;

    // Build the outer branch that produces the set of local key values.
    auto [localKeySlot, outerRootStage] = buildKeySet(state,
                                                      JoinSide::Local,
                                                      std::move(localStage),
                                                      localRecordSlot,
                                                      localFieldName,
                                                      collatorSlot,
                                                      nodeId);

    // Build the inner branch that will get the foreign key values, compare them to the local key
    // values and accumulate all matching foreign records into an array that is placed into
    // 'matchedRecordsSlot'.
    auto [matchedRecordsSlot, innerRootStage] = buildForeignMatches(localKeySlot,
                                                                    std::move(foreignStage),
                                                                    foreignRecordSlot,
                                                                    foreignFieldName,
                                                                    nodeId,
                                                                    state,
                                                                    hasUnwindSrc);

    // 'innerRootStage' should not participate in trial run tracking as the number of reads that
    // it performs should not influence planning decisions made for 'outerRootStage'.
    innerRootStage->disableTrialRunTracking();

    // Connect the two branches with a nested loop join. For each outer record with a corresponding
    // value in the 'localKeySlot', the inner branch will be executed and will place the result into
    // 'matchedRecordsSlot'.
    SbStage nlj = b.makeLoopJoin(std::move(outerRootStage),
                                 std::move(innerRootStage),
                                 buildLocalSlots(state, localRecordSlot),
                                 SbExpr::makeSV(localKeySlot) /* outerCorrelated */);

    return {matchedRecordsSlot, std::move(nlj)};
}

/*
 * Build $lookup stage using index join strategy. Below is an example plan for the aggregation
 * [{$lookup: {localField: "a", foreignField: "b"}}] with an index {b: 1} on the foreign
 * collection. Note that parts reading the local values and constructing the resulting document are
 * omitted.
 *
 * nlj [foreignDocument] [foreignDocument]
 * left
 *   nlj
 *   left
 *     nlj [lowKey, highKey]
 *     left
 *       nlj
 *       left
 *         unwind localKeySet localValue
 *         limit 1
 *         coscan
 *       right
 *         project lowKey = ks (1, 0, valueForIndexBounds, 1),
 *                 highKey = ks (1, 0, valueForIndexBounds, 2)
 *         union [valueForIndexBounds] [
 *           cfilter {isArray (localValue)}
 *           project [valueForIndexBounds = fillEmpty (getElement (localValue, 0), undefined)]
 *           limit 1
 *           coscan
 *           ,
 *           project [valueForIndexBounds = localValue]
 *           limit 1
 *           coscan
 *         ]
 *     right
 *       ixseek lowKey highKey recordId @"b_1"
 *   right
 *     limit 1
 *     seek s21 foreignDocument recordId @"foreign collection"
 * right
 *   limit 1
 *   filter {isMember (foreignValue, localValueSet)}
 *   // Below is the tree performing path traversal on the 'foreignDocument' and producing value
 *   // into 'foreignValue'.
 */
std::pair<SbSlot, SbStage> buildIndexJoinLookupStage(
    StageBuilderState& state,
    SbStage localStage,
    SbSlot localRecordSlot,
    const FieldPath& localFieldName,
    const FieldPath& foreignFieldName,
    const CollectionPtr& foreignColl,
    const IndexEntry& index,
    boost::optional<sbe::value::SlotId> collatorSlot,
    const PlanNodeId nodeId,
    bool hasUnwindSrc) {
    SbBuilder b(state, nodeId);

    CurOp::get(state.opCtx)->debug().indexedLoopJoin += 1;

    const auto foreignCollUUID = foreignColl->uuid();
    const auto foreignCollDbName = foreignColl->ns().dbName();
    const auto indexName = index.identifier.catalogName;
    const auto indexDescriptor =
        foreignColl->getIndexCatalog()->findIndexByName(state.opCtx, indexName);
    tassert(6447401,
            str::stream() << "Index " << indexName
                          << " is unexpectedly missing for $lookup index join",
            indexDescriptor);
    const auto indexAccessMethod =
        foreignColl->getIndexCatalog()->getEntry(indexDescriptor)->accessMethod()->asSortedData();
    const auto indexVersion = indexAccessMethod->getSortedDataInterface()->getKeyStringVersion();
    const auto indexOrdering = indexAccessMethod->getSortedDataInterface()->getOrdering();

    // Build the outer branch that produces the correlated local key slot.
    auto [localKeysSetSlot, localKeysSetStage] = buildKeySet(state,
                                                             JoinSide::Local,
                                                             std::move(localStage),
                                                             localRecordSlot,
                                                             localFieldName,
                                                             collatorSlot,
                                                             nodeId);

    // Unwind local keys one by one into 'singleLocalValueSlot'.
    constexpr bool preserveNullAndEmptyArrays = true;

    auto [unwindLocalKeysStage, singleLocalValueSlot, _] =
        b.makeUnwind(b.makeLimitOneCoScanTree(), localKeysSetSlot, preserveNullAndEmptyArrays);

    // We need to lookup value in 'singleLocalValueSlot' in the index defined on the foreign
    // collection. To do this, we need to generate set of point intervals corresponding to this
    // value. Single value can correspond to multiple point intervals:
    // - Array values:
    //   a. If array is empty, [Undefined, Undefined]
    //   b. If array is NOT empty, [array[0], array[0]] (point interval composed from the first
    //      array element). This is needed to match {_id: 0, a: [[1, 2]]} to {_id: 0, b: [1, 2]}.
    // - All other types, including array itself as a value, single point interval [value, value].
    //   This is needed for arrays to match {_id: 1, a: [[1, 2]]} to {_id: 0, b: [[1, 2], 42]}.
    //
    // To implement these rules, we use the union stage:
    //   union pointValue [
    //       // Branch 1
    //       filter isArray(rawValue) && !isMember(pointValue, localKeyValueSet)
    //       project pointValue = fillEmpty(
    //           getElement(rawValue, 0),
    //           Undefined
    //       )
    //       limit 1
    //       coscan
    //       ,
    //       // Branch 2
    //       project pointValue = rawValue
    //       limit 1
    //       coscan
    //   ]
    //
    // For array values, branches (1) and (2) both produce values. For all other types, only (2)
    // produces a value.
    auto [arrayBranch, arrayBranchOutSlots] = b.makeProject(
        b.makeLimitOneCoScanTree(),
        b.makeBinaryOp(sbe::EPrimBinary::fillEmpty,
                       b.makeFunction("getElement", singleLocalValueSlot, b.makeInt32Constant(0)),
                       b.makeConstant(sbe::value::TypeTags::bsonUndefined, 0)));
    SbSlot arrayBranchOutput = arrayBranchOutSlots[0];

    auto shouldProduceSeekForArray = b.makeBinaryOp(
        sbe::EPrimBinary::logicAnd,
        b.makeFunction("isArray", singleLocalValueSlot),
        b.makeUnaryOp(sbe::EPrimUnary::logicNot,
                      b.makeFunction("isMember", arrayBranchOutput, localKeysSetSlot)));
    arrayBranch = b.makeFilter(std::move(arrayBranch), std::move(shouldProduceSeekForArray));

    auto [valueBranch, valueBranchOutSlots] = b.makeProject(
        b.makeLimitOneCoScanTree(), std::pair(SbExpr{singleLocalValueSlot}, state.slotId()));
    SbSlot valueBranchOutput = valueBranchOutSlots[0];

    auto [valueGeneratorStage, unionOutputSlots] = b.makeUnion(
        sbe::makeSs(std::move(arrayBranch), std::move(valueBranch)),
        makeVector(SbExpr::makeSV(arrayBranchOutput), SbExpr::makeSV(valueBranchOutput)));

    auto valueForIndexBounds = unionOutputSlots[0];

    if (index.type == INDEX_HASHED) {
        // For hashed indexes, we need to hash the value before computing keystrings iff the
        // lookup's "foreignField" is the hashed field in this index.
        const BSONElement elt = index.keyPattern.getField(foreignFieldName.fullPath());
        if (elt.valueStringDataSafe() == IndexNames::HASHED) {
            SbSlot rawValueSlot = valueForIndexBounds;
            SbSlot indexValueSlot = rawValueSlot;

            if (collatorSlot) {
                // For collated hashed indexes, apply collation before hashing.
                auto [outStage, outSlots] = b.makeProject(
                    std::move(valueGeneratorStage),
                    b.makeFunction("collComparisonKey", rawValueSlot, SbSlot{*collatorSlot}));
                valueGeneratorStage = std::move(outStage);

                indexValueSlot = outSlots[0];
            }

            auto [outStage, outSlots] = b.makeProject(std::move(valueGeneratorStage),
                                                      b.makeFunction("shardHash", indexValueSlot));
            valueGeneratorStage = std::move(outStage);

            valueForIndexBounds = outSlots[0];
        }
    }

    // Calculate the low key and high key of each individual local field. They are stored in
    // 'lowKeySlot' and 'highKeySlot', respectively. These two slots will be made available in
    // the loop join stage to perform index seek. We also set the 'indexKeyPatternSlot' constant
    // for the seek stage later to perform consistency check.
    auto [indexKeyPaternTag, indexKeyPatternValue] =
        sbe::value::copyValue(sbe::value::TypeTags::bsonObject,
                              sbe::value::bitcastFrom<const char*>(index.keyPattern.objdata()));

    auto makeNewKeyStringCall = [&](key_string::Discriminator discriminator) {
        StringData functionName = "ks";

        SbExpr::Vector args =
            SbExpr::makeSeq(b.makeInt64Constant(static_cast<int64_t>(indexVersion)),
                            b.makeInt32Constant(indexOrdering.getBits()),
                            valueForIndexBounds,
                            b.makeInt64Constant(static_cast<int64_t>(discriminator)));

        if (collatorSlot) {
            functionName = "collKs";
            args.emplace_back(SbSlot{*collatorSlot});
        }

        return b.makeFunction(functionName, std::move(args));
    };

    auto [indexBoundKeyStage, outSlots] =
        b.makeProject(std::move(valueGeneratorStage),
                      makeNewKeyStringCall(key_string::Discriminator::kExclusiveBefore),
                      makeNewKeyStringCall(key_string::Discriminator::kExclusiveAfter),
                      b.makeConstant(indexKeyPaternTag, indexKeyPatternValue));
    SbSlot lowKeySlot = outSlots[0];
    SbSlot highKeySlot = outSlots[1];
    SbSlot indexKeyPatternSlot = outSlots[2];

    // To ensure that we compute index bounds for all local values, introduce loop join, where
    // unwinding of local values happens on the right side and index generation happens on the left
    // side.
    indexBoundKeyStage = b.makeLoopJoin(std::move(unwindLocalKeysStage),
                                        std::move(indexBoundKeyStage),
                                        {} /* outerProjects */,
                                        SbExpr::makeSV(singleLocalValueSlot) /* outerCorrelated */);

    // Perform the index seek based on the 'lowKeySlot' and 'highKeySlot' from the outer side.
    // The foreign record id of the seek is stored in 'foreignRecordIdSlot'. We also keep
    // 'indexKeySlot' and 'snapshotIdSlot' for the seek stage later to perform consistency
    // check.
    auto foreignRecordIdSlot = SbSlot{state.slotId()};
    auto indexKeySlot = SbSlot{state.slotId()};
    auto snapshotIdSlot = SbSlot{state.slotId()};
    auto indexIdentSlot = SbSlot{state.slotId()};
    auto ixScanStage =
        sbe::makeS<sbe::SimpleIndexScanStage>(foreignCollUUID,
                                              foreignCollDbName,
                                              indexName,
                                              true /* forward */,
                                              indexKeySlot.getId(),
                                              foreignRecordIdSlot.getId(),
                                              snapshotIdSlot.getId(),
                                              indexIdentSlot.getId(),
                                              sbe::IndexKeysInclusionSet{} /* indexKeysToInclude */,
                                              sbe::makeSV() /* vars */,
                                              SbExpr{lowKeySlot}.extractExpr(state),
                                              SbExpr{highKeySlot}.extractExpr(state),
                                              state.yieldPolicy,
                                              nodeId);

    // Loop join the low key and high key generation with the index seek stage to produce the
    // foreign record id to seek.
    auto ixScanNljStage =
        b.makeLoopJoin(std::move(indexBoundKeyStage),
                       std::move(ixScanStage),
                       SbExpr::makeSV(indexKeyPatternSlot) /* outerProjects */,
                       SbExpr::makeSV(lowKeySlot, highKeySlot) /* outerCorrelated */);

    // It is possible for the same record to be returned multiple times when the index is multikey
    // (contains arrays). Consider an example where local values set is '(1, 2)' and we have a
    // document with foreign field value '[1, 2]'. The same document will be returned twice:
    //  - On the first index seek, where we are looking for value '1'
    //  - On the second index seek, where we are looking for value '2'
    // To avoid such situation, we are placing 'unique' stage to prevent repeating records from
    // appearing in the result.
    if (index.multikey) {
        ixScanNljStage = b.makeUnique(std::move(ixScanNljStage), foreignRecordIdSlot);
    }

    // Loop join the foreign record id produced by the index seek on the outer side with seek
    // stage on the inner side to get matched foreign documents. The foreign documents are
    // stored in 'foreignRecordSlot'. We also pass in 'snapshotIdSlot', 'indexIdentSlot',
    // 'indexKeySlot' and 'indexKeyPatternSlot' to perform index consistency check during the seek.
    auto [scanNljStage, foreignRecordSlot, __, ___] =
        makeLoopJoinForFetch(std::move(ixScanNljStage),
                             std::vector<std::string>{},
                             foreignRecordIdSlot,
                             snapshotIdSlot,
                             indexIdentSlot,
                             indexKeySlot,
                             indexKeyPatternSlot,
                             boost::none /* prefetchedResultSlot */,
                             foreignColl,
                             state,
                             nodeId,
                             SbSlotVector{} /* slotsToForward */);

    // 'buildForeignMatches()' filters the foreign records, returned by the index scan, to match
    // those in 'localKeysSetSlot'. This is necessary because some values are encoded with the same
    // value in BTree index, such as undefined, null and empty array. In hashed indexes, hash
    // collisions are possible.
    auto [foreignGroupSlot, foreignGroupStage] = buildForeignMatches(localKeysSetSlot,
                                                                     std::move(scanNljStage),
                                                                     foreignRecordSlot,
                                                                     foreignFieldName,
                                                                     nodeId,
                                                                     state,
                                                                     hasUnwindSrc);

    // 'foreignGroupStage' should not participate in trial run tracking as the number of reads
    // that it performs should not influence planning decisions for 'localKeysSetStage'.
    foreignGroupStage->disableTrialRunTracking();

    // The top level loop join stage that joins each local field with the matched foreign
    // documents.
    auto nljStage = b.makeLoopJoin(std::move(localKeysSetStage),
                                   std::move(foreignGroupStage),
                                   buildLocalSlots(state, localRecordSlot),
                                   SbExpr::makeSV(localKeysSetSlot) /* outerCorrelated */);
    return {foreignGroupSlot, std::move(nljStage)};
}  // buildIndexJoinLookupStage

std::pair<SbSlot /*matched docs*/, SbStage> buildHashJoinLookupStage(
    StageBuilderState& state,
    SbStage localStage,
    SbSlot localRecordSlot,
    const FieldPath& localFieldName,
    SbStage foreignStage,
    SbSlot foreignRecordSlot,
    const FieldPath& foreignFieldName,
    boost::optional<sbe::value::SlotId> collatorSlot,
    const PlanNodeId nodeId,
    bool hasUnwindSrc) {
    SbBuilder b(state, nodeId);

    CurOp::get(state.opCtx)->debug().hashLookup += 1;

    // Build the outer branch that produces the set of local key values.
    auto [localKeySlot, outerRootStage] = buildKeySet(state,
                                                      JoinSide::Local,
                                                      std::move(localStage),
                                                      localRecordSlot,
                                                      localFieldName,
                                                      collatorSlot,
                                                      nodeId);

    // Build the inner branch that produces the set of foreign key values.
    auto [foreignKeySlot, foreignKeyStage] = buildKeySet(state,
                                                         JoinSide::Foreign,
                                                         std::move(foreignStage),
                                                         foreignRecordSlot,
                                                         foreignFieldName,
                                                         collatorSlot,
                                                         nodeId);

    // 'foreignKeyStage' should not participate in trial run tracking as the number of
    // reads that it performs should not influence planning decisions for 'outerRootStage'.
    foreignKeyStage->disableTrialRunTracking();

    // Build lookup stage that matches the local and foreign rows and aggregates the
    // foreign values in an array.
    auto lookupStageOutputSlot = SbSlot{state.slotId()};
    if (hasUnwindSrc) {
        // $LU [$lookup, $unwind] pattern: use HashLookupUnwindStage.
        SbStage hl = sbe::makeS<sbe::HashLookupUnwindStage>(std::move(outerRootStage),
                                                            std::move(foreignKeyStage),
                                                            localKeySlot.getId(),
                                                            foreignKeySlot.getId(),
                                                            foreignRecordSlot.getId(),
                                                            lookupStageOutputSlot.getId(),
                                                            collatorSlot,
                                                            nodeId);

        return {lookupStageOutputSlot, std::move(hl)};
    } else {
        // Plain $lookup without $unwind: use HashLookupStage.
        // Aggregator to assemble the matched foreign documents into an array.
        sbe::SlotExprPair agg =
            std::make_pair(lookupStageOutputSlot.getId(),
                           b.makeFunction("addToArray", foreignRecordSlot).extractExpr(state));

        SbStage hl = sbe::makeS<sbe::HashLookupStage>(std::move(outerRootStage),
                                                      std::move(foreignKeyStage),
                                                      localKeySlot.getId(),
                                                      foreignKeySlot.getId(),
                                                      foreignRecordSlot.getId(),
                                                      std::move(agg),
                                                      collatorSlot,
                                                      nodeId);

        // Add a projection that returns an empty array in the "as" field if no foreign row matched.
        auto [emptyArrayTag, emptyArrayVal] = sbe::value::makeNewArray();
        auto emptyArrayConstant = b.makeConstant(emptyArrayTag, emptyArrayVal);

        SbExpr innerResultProjection = b.makeBinaryOp(
            sbe::EPrimBinary::fillEmpty, lookupStageOutputSlot, std::move(emptyArrayConstant));

        auto [resultStage, outSlots] =
            b.makeProject(std::move(hl), std::move(innerResultProjection));
        SbSlot innerResultSlot = outSlots[0];

        return {innerResultSlot, std::move(resultStage)};
    }
}  // buildHashJoinLookupStage

std::pair<SbSlot /*matched docs*/, SbStage> buildLookupStage(
    StageBuilderState& state,
    EqLookupNode::LookupStrategy lookupStrategy,
    SbStage localStage,
    SbSlot localRecordSlot,
    const FieldPath& localFieldName,
    SbStage foreignStage,
    SbSlot foreignRecordSlot,
    const FieldPath& foreignFieldName,
    boost::optional<sbe::value::SlotId> collatorSlot,
    const PlanNodeId nodeId,
    bool hasUnwindSrc) {
    switch (lookupStrategy) {
        case EqLookupNode::LookupStrategy::kNestedLoopJoin:
            return buildNljLookupStage(state,
                                       std::move(localStage),
                                       localRecordSlot,
                                       localFieldName,
                                       std::move(foreignStage),
                                       foreignRecordSlot,
                                       foreignFieldName,
                                       collatorSlot,
                                       nodeId,
                                       hasUnwindSrc);
        case EqLookupNode::LookupStrategy::kHashJoin:
            return buildHashJoinLookupStage(state,
                                            std::move(localStage),
                                            localRecordSlot,
                                            localFieldName,
                                            std::move(foreignStage),
                                            foreignRecordSlot,
                                            foreignFieldName,
                                            collatorSlot,
                                            nodeId,
                                            hasUnwindSrc);
        default:
            MONGO_UNREACHABLE_TASSERT(5842606);
    }
}

/*
 * Builds a project stage that projects an empty array for each local document.
 */
std::pair<SbSlot, SbStage> buildNonExistentForeignCollLookupStage(SbStage localStage,
                                                                  const PlanNodeId nodeId,
                                                                  StageBuilderState& state) {
    SbBuilder b(state, nodeId);

    auto [emptyArrayTag, emptyArrayVal] = sbe::value::makeNewArray();
    auto emptyArrayConstant = b.makeConstant(emptyArrayTag, emptyArrayVal);
    auto [outStage, outSlots] = b.makeProject(std::move(localStage), std::move(emptyArrayConstant));
    SbSlot emptyArraySlot = outSlots[0];

    return {emptyArraySlot, std::move(outStage)};
}

std::pair<SbSlot, SbStage> buildLookupResultObject(SbStage stage,
                                                   SbSlot localDocSlot,
                                                   SbSlot resultArraySlot,
                                                   const FieldPath& fieldPath,
                                                   const PlanNodeId nodeId,
                                                   StageBuilderState& state,
                                                   bool shouldProduceBson) {
    SbBuilder b(state, nodeId);

    if (shouldProduceBson) {
        std::vector<std::string> paths;
        paths.emplace_back(fieldPath.fullPath());

        std::vector<ProjectNode> nodes;
        nodes.emplace_back(resultArraySlot);

        // We generate a projection with traversalDepth set to 0 to suppress array traversal.
        constexpr int32_t traversalDepth = 0;

        SbExpr updatedDocExpr = generateProjection(state,
                                                   projection_ast::ProjectType::kAddition,
                                                   std::move(paths),
                                                   std::move(nodes),
                                                   localDocSlot,
                                                   nullptr /* slots */,
                                                   traversalDepth);

        auto [outStage, outSlots] = b.makeProject(std::move(stage), std::move(updatedDocExpr));
        SbSlot updatedDocSlot = outSlots[0];

        return {updatedDocSlot, std::move(outStage)};
    }

    const int32_t pathLength = fieldPath.getPathLength();
    SbSlotVector fieldSlots;

    // Extract values of all fields along the path except the last one.
    for (int32_t i = 0; i < pathLength - 1; i++) {
        StringData fieldName = fieldPath.getFieldName(i);
        SbSlot inputSlot = i == 0 ? localDocSlot : fieldSlots.back();

        auto [outStage, outSlots] =
            b.makeProject(std::move(stage),
                          b.makeFunction("getField"_sd, inputSlot, b.makeStrConstant(fieldName)));

        stage = std::move(outStage);
        fieldSlots.push_back(outSlots[0]);
    }

    // Construct new objects for each path level.
    SbSlot objectSlot = resultArraySlot;

    for (int32_t i = pathLength - 1; i >= 0; i--) {
        std::string fieldName = fieldPath.getFieldName(i).toString();
        SbSlot rootObjectSlot = i == 0 ? localDocSlot : fieldSlots[i - 1];
        SbSlot valueSlot = objectSlot;

        objectSlot = SbSlot{state.slotId()};

        stage = sbe::makeS<sbe::MakeObjStage>(
            std::move(stage),
            objectSlot.getId(),                         /* objSlot */
            rootObjectSlot.getId(),                     /* rootSlot */
            sbe::MakeBsonObjStage::FieldBehavior::drop, /* fieldBehaviour */
            std::vector<std::string>{},                 /* fields */
            std::vector<std::string>{fieldName},        /* projectFields */
            sbe::value::SlotVector{valueSlot.getId()},  /* projectVars */
            true,                                       /* forceNewObject */
            false,                                      /* returnOldObject */
            nodeId);
    }

    return {objectSlot, std::move(stage)};
}
}  // namespace

/**
 * Stage builder entry point for EqLookupNode, which implements the normal MQL $lookup pattern,
 * where the query returns each local doc with all its foreign matches in an array field. This
 * supports several different lookup strategies.
 */
std::pair<SbStage, PlanStageSlots> SlotBasedStageBuilder::buildEqLookup(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    SbBuilder b(_state, root->nodeId());

    const EqLookupNode* eqLookupNode = static_cast<const EqLookupNode*>(root);
    if (eqLookupNode->lookupStrategy == EqLookupNode::LookupStrategy::kHashJoin) {
        _state.data->foreignHashJoinCollections.emplace(eqLookupNode->foreignCollection);
    }

    PlanStageReqs childReqs = reqs.copyForChild().setResultObj();
    auto [localStage, localOutputs] = build(eqLookupNode->children[0].get(), childReqs);
    SbSlot localDocSlot = localOutputs.getResultObj();

    auto [matchedDocumentsSlot, foreignStage] =
        [&, localStage = std::move(localStage)]() mutable -> std::pair<SbSlot, SbStage> {
        NamespaceString foreignNss(eqLookupNode->foreignCollection);
        auto foreignColl = _collections.lookupCollection(foreignNss);
        uassert(ErrorCodes::NamespaceNotFound,
                str::stream() << "Collection "
                              << eqLookupNode->foreignCollection.toStringForErrorMsg()
                              << " either dropped or renamed",
                (eqLookupNode->lookupStrategy ==
                 EqLookupNode::LookupStrategy::kNonExistentForeignCollection) ||
                    (foreignColl && foreignColl->ns() == foreignNss));

        boost::optional<sbe::value::SlotId> collatorSlot = _state.getCollatorSlot();

        switch (eqLookupNode->lookupStrategy) {
            // When foreign collection doesn't exist, we create stages that simply append empty
            // arrays to each local document and do not consider the case that foreign collection
            // may be created during the query, since we cannot easily create dynamic plan stages
            // and it has messier semantics. Builds a project stage that projects an empty array for
            // each local document.
            case EqLookupNode::LookupStrategy::kNonExistentForeignCollection: {
                return buildNonExistentForeignCollLookupStage(
                    std::move(localStage), eqLookupNode->nodeId(), _state);
            }
            case EqLookupNode::LookupStrategy::kIndexedLoopJoin: {
                tassert(
                    6357201,
                    "$lookup using index join should have one child and a populated index entry",
                    eqLookupNode->children.size() == 1 && eqLookupNode->idxEntry);

                return buildIndexJoinLookupStage(_state,
                                                 std::move(localStage),
                                                 localDocSlot,
                                                 eqLookupNode->joinFieldLocal,
                                                 eqLookupNode->joinFieldForeign,
                                                 foreignColl,
                                                 *eqLookupNode->idxEntry,
                                                 collatorSlot,
                                                 eqLookupNode->nodeId(),
                                                 false /* hasUnwindSrc */);
            }
            case EqLookupNode::LookupStrategy::kNestedLoopJoin:
            case EqLookupNode::LookupStrategy::kHashJoin: {
                size_t numChildren = eqLookupNode->children.size();
                tassert(6355300, "An EqLookupNode can only have one child", numChildren == 1);

                auto [foreignStage, foreignResultSlot, foreignRecordIdSlot, _] =
                    b.makeScan(foreignColl->uuid(),
                               foreignColl->ns().dbName(),
                               isForward(eqLookupNode->scanDirection));

                return buildLookupStage(_state,
                                        eqLookupNode->lookupStrategy,
                                        std::move(localStage),
                                        localDocSlot,
                                        eqLookupNode->joinFieldLocal,
                                        std::move(foreignStage),
                                        foreignResultSlot,
                                        eqLookupNode->joinFieldForeign,
                                        collatorSlot,
                                        eqLookupNode->nodeId(),
                                        false /* hasUnwindSrc */);
            }
            default:
                MONGO_UNREACHABLE_TASSERT(5842605);
        }  // switch lookupStrategy
    }();

    auto [resultSlot, resultStage] = buildLookupResultObject(std::move(foreignStage),
                                                             localDocSlot,
                                                             matchedDocumentsSlot,
                                                             eqLookupNode->joinField,
                                                             eqLookupNode->nodeId(),
                                                             _state,
                                                             eqLookupNode->shouldProduceBson);

    PlanStageSlots outputs;
    outputs.setResultObj(resultSlot);
    return {std::move(resultStage), std::move(outputs)};
}  // buildEqLookup

/**
 * Stage builder entry point for EqLookupUnwindNode, which implements the $LU ($lookup-$unwind)
 * pattern, where the query immediately unwinds the lookup result array and thus acts like a SQL
 * join. The implementation avoids materializing and then unwinding the lookup result array by
 * immediately returning a doc for each foreign match. This supports several different lookup
 * strategies.
 */
std::pair<SbStage, PlanStageSlots> SlotBasedStageBuilder::buildEqLookupUnwind(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    SbBuilder b(_state, root->nodeId());

    const EqLookupUnwindNode* eqLookupUnwindNode = static_cast<const EqLookupUnwindNode*>(root);

    // The child must produce all of the slots required by the parent of this EqLookupUnwindNode,
    // plus this node needs to produce the result slot.
    PlanStageReqs childReqs = reqs.copyForChild().setResultObj();
    auto [localStage, localOutputs] = build(eqLookupUnwindNode->children[0].get(), childReqs);
    SbSlot localDocSlot = localOutputs.get(PlanStageSlots::kResult);

    auto [matchedDocumentsSlot, foreignStage] =
        [&, localStage = std::move(localStage)]() mutable -> std::pair<SbSlot, SbStage> {
        NamespaceString foreignNss(eqLookupUnwindNode->foreignCollection);
        auto foreignColl = _collections.lookupCollection(foreignNss);
        uassert(ErrorCodes::NamespaceNotFound,
                str::stream() << "Collection "
                              << eqLookupUnwindNode->foreignCollection.toStringForErrorMsg()
                              << " either dropped or renamed",
                (eqLookupUnwindNode->lookupStrategy ==
                 EqLookupNode::LookupStrategy::kNonExistentForeignCollection) ||
                    (foreignColl && foreignColl->ns() == foreignNss));

        boost::optional<sbe::value::SlotId> collatorSlot = _state.getCollatorSlot();

        switch (eqLookupUnwindNode->lookupStrategy) {
            // When foreign collection doesn't exist, we create stages that simply append empty
            // arrays to each local document and do not consider the case that foreign collection
            // may be created during the query, since we cannot easily create dynamic plan stages
            // and it has messier semantics. Builds a project stage that projects an empty array for
            // each local document.
            case EqLookupNode::LookupStrategy::kNonExistentForeignCollection: {
                return buildNonExistentForeignCollLookupStage(
                    std::move(localStage), eqLookupUnwindNode->nodeId(), _state);
            }
            case EqLookupNode::LookupStrategy::kIndexedLoopJoin: {
                tassert(
                    8229810,
                    "$lookup using index join should have one child and a populated index entry",
                    eqLookupUnwindNode->children.size() == 1 && eqLookupUnwindNode->idxEntry);

                return buildIndexJoinLookupStage(_state,
                                                 std::move(localStage),
                                                 localDocSlot,
                                                 eqLookupUnwindNode->joinFieldLocal,
                                                 eqLookupUnwindNode->joinFieldForeign,
                                                 foreignColl,
                                                 *eqLookupUnwindNode->idxEntry,
                                                 collatorSlot,
                                                 eqLookupUnwindNode->nodeId(),
                                                 true /* hasUnwindSrc */);
            }
            case EqLookupNode::LookupStrategy::kNestedLoopJoin:
            case EqLookupNode::LookupStrategy::kHashJoin: {
                size_t numChildren = eqLookupUnwindNode->children.size();
                tassert(8229800, "An EqLookupNode can only have one child", numChildren == 1);

                auto [foreignStage, foreignResultSlot, foreignRecordIdSlot, _] =
                    b.makeScan(foreignColl->uuid(),
                               foreignColl->ns().dbName(),
                               isForward(eqLookupUnwindNode->scanDirection));

                return buildLookupStage(_state,
                                        eqLookupUnwindNode->lookupStrategy,
                                        std::move(localStage),
                                        localDocSlot,
                                        eqLookupUnwindNode->joinFieldLocal,
                                        std::move(foreignStage),
                                        foreignResultSlot,
                                        eqLookupUnwindNode->joinFieldForeign,
                                        collatorSlot,
                                        eqLookupUnwindNode->nodeId(),
                                        true /* hasUnwindSrc */);
            }
            default:
                MONGO_UNREACHABLE_TASSERT(8229801);
        }  // switch lookupStrategy
    }();

    if (eqLookupUnwindNode->lookupStrategy ==
        EqLookupNode::LookupStrategy::kNonExistentForeignCollection) {
        // Build the absorbed $unwind as our parent. We don't need to avoid materializing the lookup
        // result array since it is empty. (It would be much more difficult to detect non-existent
        // foreign collection in DocumentSourceLookup::doOptimizeAt() to avoid ever absorbing the
        // $unwind, as that computation happens later and requires several inputs that are not
        // available at the time of doOptimizeAt().)
        return buildOnlyUnwind(&(eqLookupUnwindNode->unwindNode),
                               reqs,
                               foreignStage,
                               localOutputs,
                               localDocSlot,
                               matchedDocumentsSlot);
    }

    auto [resultSlot, resultStage] = buildLookupResultObject(std::move(foreignStage),
                                                             localDocSlot,
                                                             matchedDocumentsSlot,
                                                             eqLookupUnwindNode->joinField,
                                                             eqLookupUnwindNode->nodeId(),
                                                             _state,
                                                             eqLookupUnwindNode->shouldProduceBson);

    PlanStageSlots outputs;
    outputs.set(kResult, resultSlot);
    return {std::move(resultStage), std::move(outputs)};
}  // buildEqLookupUnwind

}  // namespace mongo::stage_builder
