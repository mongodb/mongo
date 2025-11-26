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


#include "mongo/base/string_data.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/ordering.h"
#include "mongo/db/curop.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index_names.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/query/bson_typemask.h"
#include "mongo/db/query/compiler/metadata/index_entry.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/stage_builder/sbe/abt/comparison_op.h"
#include "mongo/db/query/stage_builder/sbe/builder.h"
#include "mongo/db/query/stage_builder/sbe/gen_helpers.h"
#include "mongo/db/query/stage_builder/sbe/gen_projection.h"
#include "mongo/db/query/stage_builder/sbe/sbexpr.h"
#include "mongo/db/query/stage_builder/sbe/sbexpr_helpers.h"
#include "mongo/db/query/util/make_data_structure.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_entry.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

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

// Creates an expression for traversing path 'fp' in the record from 'inputExpr' that implement MQL
// semantics for local collections. The semantics never treat terminal arrays as whole values and
// match to null per "Matching local records to null" above. Returns all the key values in a single
// array. For example, if the record in the 'inputExpr' is:
//     {a: [{b:[1,[2,3]]}, {b:4}, {b:1}, {b:2}]},
// the returned values for path "a.b" will be packed as: [1, [2,3], 4, 1, 2].
// Empty arrays and missing are skipped, that is, if the record in the 'inputExpr' is:
//     {a: [{b:1}, {b:[]}, {no_b:42}, {b:2}]},
// the returned values for path "a.b" will be packed as: [1, 2].
SbExpr generateLocalKeyStream(SbExpr inputExpr,
                              const FieldPath& fp,
                              size_t level,
                              StageBuilderState& state,
                              boost::optional<SbSlot> topLevelFieldSlot = boost::none) {
    using namespace std::literals;

    SbExprBuilder b(state);
    tassert(11051800, "FieldPath length is too short", level < fp.getPathLength());

    tassert(9033500,
            "Expected an input expression or top level field",
            !inputExpr.isNull() || topLevelFieldSlot.has_value());

    // Generate an expression to read a sub-field at the current nested level.
    SbExpr fieldExpr = topLevelFieldSlot
        ? SbExpr{*topLevelFieldSlot}
        : b.makeFunction(
              "getField"_sd, std::move(inputExpr), b.makeStrConstant(fp.getFieldName(level)));

    if (level == fp.getPathLength() - 1) {
        // In the generation of the local keys, the last level doesn't
        // expand leaf arrays.
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
                     SbExpr::makeSeq(std::move(fieldExpr),
                                     b.makeFunction("traverseP"_sd,
                                                    SbLocalVar{traverseFrameId, 0},
                                                    std::move(lambdaExpr),
                                                    b.makeInt32Constant(1))),
                     b.makeIf(b.makeFunction("isArray"_sd, SbLocalVar{traverseFrameId, 0}),
                              b.makeFunction("unwindArray"_sd, SbLocalVar{traverseFrameId, 1}),
                              SbLocalVar{traverseFrameId, 1}));
}

// Creates an expression for traversing path 'fp' in the record from 'inputSlot' that implement MQL
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
SbExpr generateForeignKeyStream(SbVar inputSlot,
                                boost::optional<SbVar> arrayPosSlot,
                                const FieldPath& fp,
                                size_t level,
                                StageBuilderState& state,
                                boost::optional<SbSlot> topLevelFieldSlot = boost::none) {
    using namespace std::literals;

    SbExprBuilder b(state);
    invariant(level < fp.getPathLength());

    // Generate an expression to read a sub-field at the current nested level.
    SbExpr getFieldFromObject;
    if (level == 0) {
        if (topLevelFieldSlot) {
            // the first navigated path is already available in the 'topLevelFieldSlot'.
            getFieldFromObject = b.makeFillEmptyNull(topLevelFieldSlot);
        } else {
            getFieldFromObject = b.makeFillEmptyNull(b.makeFunction(
                "getField"_sd, inputSlot, b.makeStrConstant(fp.getFieldName(level))));
        }
    } else {
        tassert(10800800, "arrayPosSlot must be provided", arrayPosSlot);
        // Don't get field from scalars inside arrays (it would fail but we also don't want to
        // fill with "null" in this case to match the MQL semantics described above): this is
        // achieved by checking that the position in the parent array exposed in the arrayPosSlot
        // variable is set to -1, i.e. we are not iterating over an array at all.
        SbExpr shouldGetField = b.makeBooleanOpTree(
            abt::Operations::Or,
            b.makeBinaryOp(abt::Operations::Eq, *arrayPosSlot, b.makeInt64Constant(-1)),
            b.makeFunction("isObject"_sd, inputSlot));

        getFieldFromObject =
            b.makeIf(std::move(shouldGetField),
                     b.makeFillEmptyNull(b.makeFunction(
                         "getField"_sd, inputSlot, b.makeStrConstant(fp.getFieldName(level)))),
                     b.makeNothingConstant());
    }

    if (level == fp.getPathLength() - 1) {
        // For the terminal field part, both the array elements and the array itself are considered
        // as keys.
        sbe::FrameId traverseFrameId = state.frameId();
        return b.makeLet(
            traverseFrameId,
            SbExpr::makeSeq(std::move(getFieldFromObject)),
            b.makeIf(b.makeFunction("isArray"_sd, SbLocalVar{traverseFrameId, 0}),
                     b.makeFunction("concatArrays"_sd,
                                    SbLocalVar{traverseFrameId, 0},
                                    b.makeFunction("newArray"_sd, SbLocalVar{traverseFrameId, 0})),
                     b.makeFunction("newArray"_sd, SbLocalVar{traverseFrameId, 0})));
    }

    // Generate nested traversal.
    sbe::FrameId lambdaForArrayFrameId = state.frameId();

    SbExpr lambdaForArrayExpr =
        b.makeLocalLambda2(lambdaForArrayFrameId,
                           generateForeignKeyStream(SbLocalVar{lambdaForArrayFrameId, 0},
                                                    SbLocalVar{lambdaForArrayFrameId, 1}.toVar(),
                                                    fp,
                                                    level + 1,
                                                    state));

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
    sbe::FrameId getFieldFrameId = state.frameId();
    return b.makeLet(getFieldFrameId,
                     SbExpr::makeSeq(std::move(getFieldFromObject),
                                     b.makeFunction("traverseP"_sd,
                                                    SbLocalVar{getFieldFrameId, 0},
                                                    std::move(lambdaForArrayExpr),
                                                    b.makeInt32Constant(1))),
                     b.makeIf(b.makeFunction("isArray"_sd, SbLocalVar{getFieldFrameId, 0}),
                              b.makeFunction("unwindArray"_sd, SbLocalVar{getFieldFrameId, 1}),
                              SbLocalVar{getFieldFrameId, 1}));
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

// We need to lookup the values in 'localKeyValueSet' in the index defined on the foreign
// collection. To do this, we need to generate set of point intervals corresponding to this
// value. Single value can correspond to multiple point intervals:
// - Array values:
//   a. If array is empty, [Undefined, Undefined]
//   b. If array is NOT empty, [array[0], array[0]] (point interval composed from the first
//      array element). This is needed to match {_id: 0, a: [[1, 2]]} to {_id: 0, b: [1, 2]}.
// - All other types, including array itself as a value, single point interval [value, value].
//   This is needed for arrays to match {_id: 1, a: [[1, 2]]} to {_id: 0, b: [[1, 2], 42]}.
//
// To implement these rules, we extract the first element of every array found in the set of
// local keys, append them to the set of local keys, then remove duplicates by converting it
// again to a set (the extracted value could be identical to another value already in the
// localKeyValueSet - SERVER-66119):
//   localKeyValueSet = if(
//        traverseF(localKeyValueSet, lambda(value){
//          isArray(value)
//        }, false),
//        arrayToSet(concatArrays(localKeyValueSet,
//          traverseP(localKeyValueSet, lambda(rawValue){
//            if (isArray(rawValue)) then
//              fillEmpty(
//                getElement(rawValue, 0),
//                Undefined
//              )
//            else
//              rawValue
//          }, 1)),
//        localKeyValueSet
//   )
//
// In case of non-array we add the same value, relying on the fact it will be removed by the
// deduplication
std::pair<SbSlot /* keyValuesSetSlot */, SbStage> buildKeySetForIndexScan(StageBuilderState& state,
                                                                          SbStage inputStage,
                                                                          SbSlot localKeysSetSlot,
                                                                          const PlanNodeId nodeId) {
    SbBuilder b(state, nodeId);

    auto lambdaIsArrayFrameId = state.frameId();
    SbLocalVar lambdaIsArrayVar(lambdaIsArrayFrameId, 0);
    auto lambdaIsArrayExpr =
        b.makeLocalLambda(lambdaIsArrayFrameId, b.makeFunction("isArray"_sd, lambdaIsArrayVar));

    auto lambdaFrameId = state.frameId();
    SbLocalVar lambdaVar(lambdaFrameId, 0);
    auto lambdaExpr =
        b.makeLocalLambda(lambdaFrameId,
                          b.makeIf(b.makeFunction("isArray"_sd, lambdaVar),
                                   b.makeFillEmptyUndefined(b.makeFunction(
                                       "getElement"_sd, lambdaVar, b.makeInt32Constant(0))),
                                   lambdaVar));

    SbExpr expr = b.makeIf(b.makeFunction("traverseF"_sd,
                                          localKeysSetSlot,
                                          std::move(lambdaIsArrayExpr),
                                          b.makeBoolConstant(false)),
                           b.makeFunction("arrayToSet"_sd,
                                          b.makeFunction("concatArrays"_sd,
                                                         localKeysSetSlot,
                                                         b.makeFunction("traverseP"_sd,
                                                                        localKeysSetSlot,
                                                                        std::move(lambdaExpr),
                                                                        b.makeInt32Constant(1)))),
                           localKeysSetSlot);
    auto [outStage, outSlots] = b.makeProject(std::move(inputStage), std::move(expr));

    return {outSlots[0], std::move(outStage)};
}

// Creates stages for traversing path 'fp' in the record from 'inputSlot'. Puts the set of key
// values into 'keyValuesSetSlot'. For example, if the record in the 'inputSlot' is:
//     {a: [{b:[1,[2,3]]}, {b:4}, {b:1}, {b:2}]},
// the returned slot will contain for path "a.b" a set of {1, 2, 4, [2,3]}.
// If the stream produces no values, that is, would result in an empty set, the empty set is
// replaced with a set that contains a single 'null' value, so that it matches MQL semantics when
// empty arrays and all missing are matched to 'null'.
std::pair<SbSlot /* keyValuesSetSlot */, SbStage> buildKeySetForLocal(
    StageBuilderState& state,
    SbStage inputStage,
    const PlanStageSlots& slots,
    const FieldPath& fp,
    boost::optional<sbe::value::SlotId> collatorSlot,
    const PlanNodeId nodeId) {
    SbBuilder b(state, nodeId);

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
        SbExpr::makeSeq(generateLocalKeyStream(
            slots.getResultObj(),
            fp,
            0,
            state,
            slots.getIfExists(std::make_pair(PlanStageSlots::kField, fp.getFieldName(0))))),
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

    auto [outStage, outSlots] =
        b.makeProject(buildVariableTypes(slots), std::move(inputStage), std::move(expr));
    inputStage = std::move(outStage);

    auto keyValuesSetSlot = outSlots[0];

    return {keyValuesSetSlot, std::move(inputStage)};
}

/**
 * Traverses path 'fp' in the 'inputSlot' and puts the set of key values into 'keyValuesSetSlot'.
 */
std::pair<SbSlot /* keyValuesSetSlot */, SbStage> buildKeySetForForeign(
    StageBuilderState& state,
    SbStage inputStage,
    SbSlot inputSlot,
    const FieldPath& fp,
    SbSlot topLevelFieldSlot,
    boost::optional<sbe::value::SlotId> collatorSlot,
    const PlanNodeId nodeId) {
    SbBuilder b(state, nodeId);

    SbExpr expr =
        generateForeignKeyStream(inputSlot.toVar(), boost::none, fp, 0, state, topLevelFieldSlot);
    // Convert the array into an ArraySet that has no duplicate keys.
    if (collatorSlot) {
        expr = b.makeFunction("collArrayToSet"_sd, SbSlot{*collatorSlot}, std::move(expr));
    } else {
        expr = b.makeFunction("arrayToSet"_sd, std::move(expr));
    }

    auto [outStage, outSlots] = b.makeProject(
        buildVariableTypes(topLevelFieldSlot), std::move(inputStage), std::move(expr));
    auto keyValuesSetSlot = outSlots[0];

    return {keyValuesSetSlot, std::move(outStage)};
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

    auto concatArraysExpr =
        b.makeFunction("aggConcatArraysCapped", spillSlot, b.makeInt32Constant(sizeCap));

    SbHashAggAccumulatorVector accumulatorList;
    accumulatorList.emplace_back(SbHashAggAccumulator{
        .outSlot = boost::none,  // A slot will be assigned when creating the final HashAgg stage.
        .spillSlot = spillSlot,
        .implementation =
            SbHashAggCompiledAccumulator{
                .init = SbExpr{},
                .agg = std::move(addToArrayExpr),
                .merge = std::move(concatArraysExpr),
            },
    });

    auto [hashAggStage, _, aggOutSlots] = b.makeHashAgg(VariableTypes{},
                                                        std::move(innerBranch),
                                                        {}, /* groupBy slots */
                                                        accumulatorList,
                                                        {});
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
 *   branch0 [projOutputSlot]
 *     project [projOutputSlot = getElement(groupSlot, 0)]
 *     group [] [groupSlot = addToArrayCapped(foreignRecordSlot, 104857600)]
 *     filter {traverseF (
 *       let [
 *           l11.0 = fillEmpty (topLevelFieldSlot, null)
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
 *     scan foreignRecordSlot recordIdSlot none none none none [topLevelFieldSlot = "a"] @uuid true
 *          false
 *   branch1 [emptySlot]
 *     project [emptySlot = []]
 *     limit 1
 *     coscan
 * ]
 */
std::pair<SbSlot /* matched docs */, SbStage> buildForeignMatches(SbSlot localKeySlot,
                                                                  SbStage foreignStage,
                                                                  SbSlot foreignRecordSlot,
                                                                  const FieldPath& foreignFieldName,
                                                                  SbSlot topLevelFieldSlot,
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

        auto getFieldOrNull = b.makeFillEmptyNull(
            i == 0 ? topLevelFieldSlot
                   : b.makeFunction("getField"_sd,
                                    lambdaArg.clone(),
                                    b.makeStrConstant(foreignFieldName.getFieldName(i))));

        // Non object/array field will be converted into Nothing, passing along recursive traverseF
        // and will be treated as null to compared against local key set.
        if (i != foreignPathLength - 1) {
            auto localBindFrameId = state.frameId();

            auto binds = SbExpr::makeSeq(std::move(getFieldOrNull));
            SbVar var = SbVar{localBindFrameId, 0};

            auto innerExpr =
                b.makeIf(b.makeFunction("typeMatch"_sd,
                                        var,
                                        b.makeInt32Constant(getBSONTypeMask(BSONType::array) |
                                                            getBSONTypeMask(BSONType::object))),
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
            filter = b.makeIf(b.makeFillEmptyTrue(b.makeFunction("isObject"_sd, lambdaArg.clone())),
                              std::move(filter),
                              b.makeBoolConstant(false));
        }
    }

    SbStage foreignOutputStage = b.makeFilter(
        buildVariableTypes(topLevelFieldSlot), std::move(foreignStage), std::move(filter));
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
    const PlanStageSlots& slots,
    const FieldPath& localFieldName,
    const CollectionPtr& foreignColl,
    const FieldPath& foreignFieldName,
    boost::optional<sbe::value::SlotId> collatorSlot,
    const PlanNodeId nodeId,
    bool hasUnwindSrc,
    bool forwardScanDirection) {
    SbBuilder b(state, nodeId);

    CurOp::get(state.opCtx)->debug().nestedLoopJoin += 1;

    // Build the outer branch that produces the set of local key values.
    auto [localKeySlot, outerRootStage] = buildKeySetForLocal(
        state, std::move(localStage), slots, localFieldName, collatorSlot, nodeId);

    auto [foreignStage, foreignRecordSlot, _, scanFieldSlots] =
        b.makeScan(foreignColl->uuid(),
                   foreignColl->ns().dbName(),
                   forwardScanDirection,
                   std::vector<std::string>{std::string(foreignFieldName.front())});

    // Build the inner branch that will get the foreign key values, compare them to the local key
    // values and accumulate all matching foreign records into an array that is placed into
    // 'matchedRecordsSlot'.
    auto [matchedRecordsSlot, innerRootStage] = buildForeignMatches(localKeySlot,
                                                                    std::move(foreignStage),
                                                                    foreignRecordSlot,
                                                                    foreignFieldName,
                                                                    scanFieldSlots[0],
                                                                    nodeId,
                                                                    state,
                                                                    hasUnwindSrc);

    // 'innerRootStage' should not participate in trial run tracking as the number of reads that
    // it performs should not influence planning decisions made for 'outerRootStage'.
    innerRootStage->disableTrialRunTracking();

    SbSlot localRecordSlot = slots.getResultObj();
    // Connect the two branches with a nested loop join. For each outer record with a corresponding
    // value in the 'localKeySlot', the inner branch will be executed and will place the result into
    // 'matchedRecordsSlot'.
    SbStage nlj = b.makeLoopJoin(std::move(outerRootStage),
                                 std::move(innerRootStage),
                                 buildLocalSlots(state, localRecordSlot),
                                 SbExpr::makeSV(localKeySlot) /* outerCorrelated */);

    return {matchedRecordsSlot, std::move(nlj)};
}


std::tuple<SbStage, SbSlot, SbSlot, SbSlotVector> buildIndexJoinLookupForeignSideStage(
    StageBuilderState& state,
    SbSlot localKeysSetSlot,
    const FieldPath& localFieldName,
    const FieldPath& foreignFieldName,
    const CollectionPtr& foreignColl,
    const IndexEntry& index,
    boost::optional<sbe::value::SlotId> collatorSlot,
    const PlanNodeId nodeId,
    bool hasUnwindSrc) {
    SbBuilder b(state, nodeId);

    const auto foreignCollUUID = foreignColl->uuid();
    const auto& foreignCollDbName = foreignColl->ns().dbName();
    const auto& indexName = index.identifier.catalogName;
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

    // Modify the set of values to lookup to include the first item of any array.
    auto [localKeysIndexSetSlot, localKeysSetStage] =
        buildKeySetForIndexScan(state, b.makeLimitOneCoScanTree(), localKeysSetSlot, nodeId);

    // Unwind local keys one by one into 'valueForIndexBounds'.
    auto [valueGeneratorStage, valueForIndexBounds, _] = b.makeUnwind(
        std::move(localKeysSetStage), localKeysIndexSetSlot, true /*preserveNullAndEmptyArrays*/);

    if (index.type == INDEX_HASHED) {
        // For hashed indexes, we need to hash the value before computing keystrings iff the
        // lookup's "foreignField" is the hashed field in this index.
        const BSONElement elt = index.keyPattern.getField(foreignFieldName.fullPath());
        if (elt.valueStringDataSafe() == IndexNames::HASHED) {

            // For collated hashed indexes, apply collation before hashing.
            auto [outStage, outSlots] =
                b.makeProject(std::move(valueGeneratorStage),
                              b.makeFunction("shardHash"_sd,
                                             collatorSlot ? b.makeFunction("collComparisonKey",
                                                                           valueForIndexBounds,
                                                                           SbSlot{*collatorSlot})
                                                          : valueForIndexBounds));
            valueGeneratorStage = std::move(outStage);
            valueForIndexBounds = outSlots[0];
        }
    }

    // Calculate the low key and high key of each individual local field. They are stored in
    // 'lowKeySlot' and 'highKeySlot', respectively. These two slots will be made available in
    // the loop join stage to perform index seek.
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
                      makeNewKeyStringCall(key_string::Discriminator::kExclusiveAfter));
    SbSlot lowKeySlot = outSlots[0];
    SbSlot highKeySlot = outSlots[1];

    auto indexInfoTypeMask = SbIndexInfoType::kIndexIdent | SbIndexInfoType::kIndexKey |
        SbIndexInfoType::kIndexKeyPattern | SbIndexInfoType::kSnapshotId;

    // Perform the index seek based on the 'lowKeySlot' and 'highKeySlot' from the outer side.
    // The foreign record id of the seek is stored in 'foreignRecordIdSlot'. We also keep
    // 'indexKeySlot' and 'snapshotIdSlot' for the seek stage later to perform consistency
    // check.
    auto [ixScanStage, foreignRecordIdSlot, __, indexInfoSlots] =
        b.makeSimpleIndexScan(foreignCollUUID,
                              foreignCollDbName,
                              indexName,
                              index.keyPattern,
                              true /* forward */,
                              lowKeySlot,
                              highKeySlot,
                              sbe::IndexKeysInclusionSet{} /* indexKeysToInclude */,
                              indexInfoTypeMask);

    SbSlot indexIdentSlot = *indexInfoSlots.indexIdentSlot;
    SbSlot indexKeySlot = *indexInfoSlots.indexKeySlot;
    SbSlot indexKeyPatternSlot = *indexInfoSlots.indexKeyPatternSlot;
    SbSlot snapshotIdSlot = *indexInfoSlots.snapshotIdSlot;

    // Loop join the low key and high key generation with the index seek stage to produce the
    // foreign record id to seek.
    auto ixScanNljStage =
        b.makeLoopJoin(std::move(indexBoundKeyStage),
                       std::move(ixScanStage),
                       SbSlotVector{} /* outerProjects */,
                       SbExpr::makeSV(lowKeySlot, highKeySlot) /* outerCorrelated */);

    // It is possible for the same record to be returned multiple times when the index is multikey
    // (contains arrays). Consider an example where local values set is '(1, 2)' and we have a
    // document with foreign field value '[1, 2]'. The same document will be returned twice:
    //  - On the first index seek, where we are looking for value '1'
    //  - On the second index seek, where we are looking for value '2'
    // To avoid such situation, we are placing 'unique' stage to prevent repeating records from
    // appearing in the result.
    if (index.multikey) {
        if (foreignColl->isClustered()) {
            ixScanNljStage = b.makeUnique(std::move(ixScanNljStage), foreignRecordIdSlot);
        } else {
            ixScanNljStage = b.makeUniqueRoaring(std::move(ixScanNljStage), foreignRecordIdSlot);
        }
    }

    // Loop join the foreign record id produced by the index seek on the outer side with seek
    // stage on the inner side to get matched foreign documents. The foreign documents are
    // stored in 'foreignRecordSlot'. We also pass in 'snapshotIdSlot', 'indexIdentSlot',
    // 'indexKeySlot' and 'indexKeyPatternSlot' to perform index consistency check during the seek.
    auto [scanNljStage, scanNljValueSlot, scanNljRecordIdSlot, scanNljFieldSlots] =
        makeLoopJoinForFetch(std::move(ixScanNljStage),
                             std::vector<std::string>{std::string(foreignFieldName.front())},
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

    return {std::move(scanNljStage),
            scanNljValueSlot,
            scanNljRecordIdSlot,
            std::move(scanNljFieldSlots)};
}  // buildIndexJoinLookupForeignSideStage

/*
 * Build $lookup stage using index join strategy. Below is an example plan for the aggregation
 * [{$lookup: {localField: "a", foreignField: "b"}}] with an index {b: 1} on the foreign
 * collection. Note that parts reading the local values and constructing the resulting document are
 * omitted.
 *
 *   filter {isMember (foreignValue, localValueSet)}
 *   nlj
 *   left
 *     nlj [lowKey, highKey]
 *     left
 *       project lowKey = ks (1, 0, valueForIndexBounds, 1),
 *               highKey = ks (1, 0, valueForIndexBounds, 2)
 *       unwind localKeySet localValue
 *       limit 1
 *       coscan
 *     right
 *       ixseek lowKey highKey recordId @"b_1"
 *   right
 *     limit 1
 *     seek s21 foreignDocument recordId [foreignValue = "b"] @"foreign collection"
 */
std::pair<SbSlot, SbStage> buildIndexJoinLookupStage(
    StageBuilderState& state,
    SbStage localStage,
    const PlanStageSlots& slots,
    const FieldPath& localFieldName,
    const FieldPath& foreignFieldName,
    const CollectionPtr& foreignColl,
    const IndexEntry& index,
    boost::optional<sbe::value::SlotId> collatorSlot,
    const PlanNodeId nodeId,
    bool hasUnwindSrc) {
    SbBuilder b(state, nodeId);

    CurOp::get(state.opCtx)->debug().indexedLoopJoin += 1;

    // Build the outer branch that produces the correlated local key slot.
    auto [localKeysSetSlot, localKeysSetStage] = buildKeySetForLocal(
        state, std::move(localStage), slots, localFieldName, collatorSlot, nodeId);

    // Build the inner branch that produces the correlated foreign key slot.
    auto [scanNljStage, foreignRecordSlot, _, scanFieldSlots] =
        buildIndexJoinLookupForeignSideStage(state,
                                             localKeysSetSlot,
                                             localFieldName,
                                             foreignFieldName,
                                             foreignColl,
                                             index,
                                             collatorSlot,
                                             nodeId,
                                             hasUnwindSrc);

    // 'buildForeignMatches()' filters the foreign records, returned by the index scan, to match
    // those in 'localKeysSetSlot'. This is necessary because some values are encoded with the same
    // value in BTree index, such as undefined, null and empty array. In hashed indexes, hash
    // collisions are possible.
    auto [foreignGroupSlot, foreignGroupStage] = buildForeignMatches(localKeysSetSlot,
                                                                     std::move(scanNljStage),
                                                                     foreignRecordSlot,
                                                                     foreignFieldName,
                                                                     scanFieldSlots[0],
                                                                     nodeId,
                                                                     state,
                                                                     hasUnwindSrc);

    // 'foreignGroupStage' should not participate in trial run tracking as the number of reads
    // that it performs should not influence planning decisions for 'localKeysSetStage'.
    foreignGroupStage->disableTrialRunTracking();

    SbSlot localRecordSlot = slots.getResultObj();
    // The top level loop join stage that joins each local field with the matched foreign
    // documents.
    auto nljStage = b.makeLoopJoin(std::move(localKeysSetStage),
                                   std::move(foreignGroupStage),
                                   buildLocalSlots(state, localRecordSlot),
                                   SbExpr::makeSV(localKeysSetSlot) /* outerCorrelated */);
    return {foreignGroupSlot, std::move(nljStage)};
}  // buildIndexJoinLookupStage

/*
 * Build $lookup stage using dynamic index join strategy. The dynamic index join strategy decides
 * whether an index can be used to access the foreign relation at runtime. For each record on the
 * local collation, the algorithm checks the type of the local key, if the type is not String, or
 * Object, or Array, it accesses the record using the index of the foreign collection, otherwise it
 * scans the foreign collection. Below is an example plan for the aggregation [{$lookup:
 * {localField: "a", foreignField: "b"}}] with an index {b: 1} on the foreign collection. Note that
 * parts reading the local values and constructing the resulting document are omitted.
 *
 * nlj localDocument localKeySet
 * left
 *     build localKeySet
 * right
 *     if localKey is NOT (String or Array or Object)
 *         nlj
 *         left
 *             nlj [lowKey, highKey]
 *             left
 *                 project lowKey = ks (1, 0, valueForIndexBounds, 1),
 *                         highKey = ks (1, 0, valueForIndexBounds, 2)
 *                 unwind localKeySet localValue
 *                 limit 1
 *                 coscan
 *             right
 *                 ixseek lowKey highKey recordId @"b_1"
 *         right
 *             limit 1
 *             seek s12 foreignDocument recordId @"foreign collection"
 *     else
 *       scan "foreign collection"
 */
std::pair<SbSlot, SbStage> buildDynamicIndexedLoopJoinLookupStage(
    StageBuilderState& state,
    SbStage localStage,
    const PlanStageSlots& slots,
    const FieldPath& localFieldName,
    const CollectionPtr& foreignColl,
    const FieldPath& foreignFieldName,
    const IndexEntry& index,
    boost::optional<sbe::value::SlotId> collatorSlot,
    const PlanNodeId nodeId,
    bool hasUnwindSrc,
    bool forwardScanDirection) {

    SbBuilder b(state, nodeId);

    CurOp::get(state.opCtx)->debug().dynamicIndexedLoopJoin += 1;

    // Build the index Lookup branch
    auto [localKeysSetSlot, localKeysSetStage] = buildKeySetForLocal(
        state, std::move(localStage), slots, localFieldName, collatorSlot, nodeId);

    auto [indexLookupBranchStage,
          indexLookupBranchResultSlot,
          indexLookupBranchRecordIdSlot,
          indexLookupBranchScanSlots] = buildIndexJoinLookupForeignSideStage(state,
                                                                             localKeysSetSlot,
                                                                             localFieldName,
                                                                             foreignFieldName,
                                                                             foreignColl,
                                                                             index,
                                                                             collatorSlot,
                                                                             nodeId,
                                                                             hasUnwindSrc);

    // Build the nested loop branch.
    auto [nestedLoopBranchStage,
          nestedLoopBranchResultSlot,
          nestedLoopBranchRecordIdSlot,
          nestedLoopBranchScanSlots] =
        b.makeScan(foreignColl->uuid(),
                   foreignColl->ns().dbName(),
                   forwardScanDirection,
                   std::vector<std::string>{std::string(foreignFieldName.front())});

    // Build the typeMatch filter expression
    sbe::FrameId frameId = state.frameId();
    auto lambdaArg = SbExpr{SbVar{frameId, 0}};
    SbExpr typeMatchLambdaFilter = b.makeFunction(
        "typeMatch"_sd,
        lambdaArg.clone(),
        b.makeInt32Constant(getBSONTypeMask(BSONType::string) | getBSONTypeMask(BSONType::array) |
                            getBSONTypeMask(BSONType::object)));
    auto typeMatchLambdaFunction = b.makeLocalLambda(frameId, std::move(typeMatchLambdaFilter));
    auto filter = b.makeNot(b.makeFunction("traverseF"_sd,
                                           SbExpr{localKeysSetSlot},
                                           std::move(typeMatchLambdaFunction),
                                           b.makeBoolConstant(false) /*compareArray*/));

    // Create a branch stage
    auto [branchStage, branchSlots] = b.makeBranch(std::move(indexLookupBranchStage),
                                                   std::move(nestedLoopBranchStage),
                                                   std::move(filter),
                                                   SbExpr::makeSV(indexLookupBranchResultSlot,
                                                                  indexLookupBranchRecordIdSlot,
                                                                  indexLookupBranchScanSlots[0]),
                                                   SbExpr::makeSV(nestedLoopBranchResultSlot,
                                                                  nestedLoopBranchRecordIdSlot,
                                                                  nestedLoopBranchScanSlots[0]));

    SbSlot resultSlot = branchSlots[0];
    auto [finalForeignSlot, finalForeignStage] = buildForeignMatches(localKeysSetSlot,
                                                                     std::move(branchStage),
                                                                     resultSlot,
                                                                     foreignFieldName,
                                                                     branchSlots[2],
                                                                     nodeId,
                                                                     state,
                                                                     hasUnwindSrc);

    //  'finalForeignStage' should not participate in trial run tracking as the number of
    //  reads that it performs should not influence planning decisions for 'outerRootStage'.
    finalForeignStage->disableTrialRunTracking();

    SbSlot localRecordSlot = slots.getResultObj();
    // Connect the local (left) and foreign (right) sides with a nested loop join. For each left
    // record with a corresponding value in the 'localKeySlot', the right branch will be executed
    // and will place the result into 'matchedRecordsSlot'.
    SbStage nlj =
        b.makeLoopJoin(std::move(localKeysSetStage),
                       std::move(finalForeignStage),
                       buildLocalSlots(state, localRecordSlot),
                       SbExpr::makeSV(localKeysSetSlot, localRecordSlot) /* outerCorrelated */);

    return {finalForeignSlot, std::move(nlj)};

}  // buildDynamicIndexedLoopJoinLookupStage

std::pair<SbSlot /*matched docs*/, SbStage> buildHashJoinLookupStage(
    StageBuilderState& state,
    SbStage localStage,
    const PlanStageSlots& slots,
    const FieldPath& localFieldName,
    const CollectionPtr& foreignColl,
    const FieldPath& foreignFieldName,
    boost::optional<sbe::value::SlotId> collatorSlot,
    const PlanNodeId nodeId,
    bool hasUnwindSrc,
    bool forwardScanDirection) {
    SbBuilder b(state, nodeId);

    CurOp::get(state.opCtx)->debug().hashLookup += 1;

    // Build the outer branch that produces the correlated local key slot.
    auto [localKeysSetSlot, localKeysSetStage] = buildKeySetForLocal(
        state, std::move(localStage), slots, localFieldName, collatorSlot, nodeId);


    // Build the inner branch that produces the set of foreign key values.
    auto [foreignStage, foreignRecordSlot, foreignRecordIdSlot, scanFieldSlots] =
        b.makeScan(foreignColl->uuid(),
                   foreignColl->ns().dbName(),
                   forwardScanDirection,
                   std::vector<std::string>{std::string(foreignFieldName.front())});

    auto [foreignKeySlot, foreignKeyStage] = buildKeySetForForeign(state,
                                                                   std::move(foreignStage),
                                                                   foreignRecordSlot,
                                                                   foreignFieldName,
                                                                   scanFieldSlots[0],
                                                                   collatorSlot,
                                                                   nodeId);

    // 'foreignKeyStage' should not participate in trial run tracking as the number of
    // reads that it performs should not influence planning decisions for 'outerRootStage'.
    foreignKeyStage->disableTrialRunTracking();

    // Build lookup stage that matches the local and foreign rows and aggregates the
    // foreign values in an array.
    if (hasUnwindSrc) {
        auto [hl, lookupStageOutputSlot] = b.makeHashLookupUnwind(std::move(localKeysSetStage),
                                                                  std::move(foreignKeyStage),
                                                                  localKeysSetSlot,
                                                                  foreignKeySlot,
                                                                  foreignRecordSlot,
                                                                  collatorSlot);
        return {lookupStageOutputSlot, std::move(hl)};
    } else {
        // Plain $lookup without $unwind: use HashLookupStage.
        // Aggregator to assemble the matched foreign documents into an array.
        SbBlockAggExpr agg{SbExpr{} /*init*/,
                           SbExpr{} /*blockAgg*/,
                           b.makeFunction("addToArray", foreignRecordSlot) /*agg*/};

        auto [hl, lookupStageOutputSlot] = b.makeHashLookup(std::move(localKeysSetStage),
                                                            std::move(foreignKeyStage),
                                                            localKeysSetSlot,
                                                            foreignKeySlot,
                                                            foreignRecordSlot,
                                                            std::move(agg),
                                                            boost::none /* optOutputSlot */,
                                                            collatorSlot);

        // Add a projection that returns an empty array in the "as" field if no foreign row matched.
        auto [emptyArrayTag, emptyArrayVal] = sbe::value::makeNewArray();
        auto emptyArrayConstant = b.makeConstant(emptyArrayTag, emptyArrayVal);

        SbExpr innerResultProjection =
            b.makeFillEmpty(lookupStageOutputSlot, std::move(emptyArrayConstant));

        auto [resultStage, outSlots] =
            b.makeProject(std::move(hl), std::move(innerResultProjection));
        SbSlot innerResultSlot = outSlots[0];

        return {innerResultSlot, std::move(resultStage)};
    }
}  // buildHashJoinLookupStage

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

std::pair<SbSlot /*matched docs*/, SbStage> buildLookupStage(
    StageBuilderState& state,
    EqLookupNode::LookupStrategy lookupStrategy,
    SbStage localStage,
    const PlanStageSlots& slots,
    const FieldPath& localFieldName,
    const FieldPath& foreignFieldName,
    const CollectionPtr& foreignColl,
    boost::optional<IndexEntry> index,
    boost::optional<sbe::value::SlotId> collatorSlot,
    const PlanNodeId nodeId,
    size_t numChildren,
    bool hasUnwindSrc,
    bool forwardScanDirection) {
    SbBuilder b(state, nodeId);

    switch (lookupStrategy) {
        // When foreign collection doesn't exist, we create stages that simply append empty
        // arrays to each local document and do not consider the case that foreign collection
        // may be created during the query, since we cannot easily create dynamic plan stages
        // and it has messier semantics. Builds a project stage that projects an empty array for
        // each local document.
        case EqLookupNode::LookupStrategy::kNonExistentForeignCollection: {
            return buildNonExistentForeignCollLookupStage(std::move(localStage), nodeId, state);
        }
        case EqLookupNode::LookupStrategy::kIndexedLoopJoin: {
            tassert(6357201,
                    "$lookup using index join should have one child and a populated index entry",
                    numChildren == 1 && index);

            return buildIndexJoinLookupStage(state,
                                             std::move(localStage),
                                             slots,
                                             localFieldName,
                                             foreignFieldName,
                                             foreignColl,
                                             *index,
                                             collatorSlot,
                                             nodeId,
                                             hasUnwindSrc);
        }
        case mongo::EqLookupNode::LookupStrategy::kDynamicIndexedLoopJoin: {
            tassert(8155500,
                    "$lookup using dynamic indexed loop join should have one child and a populated "
                    "index entry",
                    numChildren == 1 && index);

            return buildDynamicIndexedLoopJoinLookupStage(state,
                                                          std::move(localStage),
                                                          slots,
                                                          localFieldName,
                                                          foreignColl,
                                                          foreignFieldName,
                                                          *index,
                                                          collatorSlot,
                                                          nodeId,
                                                          hasUnwindSrc,
                                                          forwardScanDirection);
        }
        case EqLookupNode::LookupStrategy::kNestedLoopJoin: {
            tassert(8155501, "A $lookup node can only have one child", numChildren == 1);

            return buildNljLookupStage(state,
                                       std::move(localStage),
                                       slots,
                                       localFieldName,
                                       foreignColl,
                                       foreignFieldName,
                                       collatorSlot,
                                       nodeId,
                                       hasUnwindSrc,
                                       forwardScanDirection);
        }
        case EqLookupNode::LookupStrategy::kHashJoin: {
            tassert(6355300, "A $lookup node can only have one child", numChildren == 1);

            return buildHashJoinLookupStage(state,
                                            std::move(localStage),
                                            slots,
                                            localFieldName,
                                            foreignColl,
                                            foreignFieldName,
                                            collatorSlot,
                                            nodeId,
                                            hasUnwindSrc,
                                            forwardScanDirection);
        }
        default:
            MONGO_UNREACHABLE_TASSERT(5842605);
    }  // switch lookupStrategy
}  // buildLookupStage

std::pair<SbSlot, SbStage> buildLookupResultObject(SbStage stage,
                                                   SbSlot localDocSlot,
                                                   std::vector<ProjectNode> nodes,
                                                   std::vector<std::string> paths,
                                                   const PlanNodeId nodeId,
                                                   StageBuilderState& state,
                                                   bool shouldProduceBson) {
    // We generate a projection with traversalDepth set to 0 to suppress array traversal.
    constexpr int32_t traversalDepth = 0;

    SbExpr updatedDocExpr = generateProjection(state,
                                               projection_ast::ProjectType::kAddition,
                                               std::move(paths),
                                               std::move(nodes),
                                               localDocSlot,
                                               nullptr /* slots */,
                                               traversalDepth,
                                               shouldProduceBson);

    SbBuilder b(state, nodeId);
    auto [outStage, outSlots] = b.makeProject(std::move(stage), std::move(updatedDocExpr));
    SbSlot updatedDocSlot = outSlots[0];

    return {updatedDocSlot, std::move(outStage)};
}

std::pair<SbSlot, SbStage> buildLookupResultObject(SbStage stage,
                                                   SbSlot localDocSlot,
                                                   SbSlot resultArraySlot,
                                                   const FieldPath& fieldPath,
                                                   const PlanNodeId nodeId,
                                                   StageBuilderState& state,
                                                   bool shouldProduceBson) {
    std::vector<std::string> paths;
    paths.emplace_back(fieldPath.fullPath());

    std::vector<ProjectNode> nodes;
    nodes.emplace_back(resultArraySlot);

    return buildLookupResultObject(std::move(stage),
                                   localDocSlot,
                                   std::move(nodes),
                                   std::move(paths),
                                   nodeId,
                                   state,
                                   shouldProduceBson);
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
    // Try to get the beginning of the localField path into a slot.
    childReqs.setFields(
        std::vector<std::string>{std::string(eqLookupNode->joinFieldLocal.front())});
    auto [localStage, localOutputs] = build(eqLookupNode->children[0].get(), childReqs);
    SbSlot localRecordSlot = localOutputs.getResultObj();

    NamespaceString foreignNss(eqLookupNode->foreignCollection);
    auto foreignColl = _collections.lookupCollection(foreignNss);
    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "Collection " << eqLookupNode->foreignCollection.toStringForErrorMsg()
                          << " either dropped or renamed",
            (eqLookupNode->lookupStrategy ==
             EqLookupNode::LookupStrategy::kNonExistentForeignCollection) ||
                (foreignColl && foreignColl->ns() == foreignNss));

    boost::optional<sbe::value::SlotId> collatorSlot = _state.getCollatorSlot();

    auto [matchedDocumentsSlot, foreignStage] =
        buildLookupStage(_state,
                         eqLookupNode->lookupStrategy,
                         std::move(localStage),
                         localOutputs,
                         eqLookupNode->joinFieldLocal,
                         eqLookupNode->joinFieldForeign,
                         foreignColl,
                         eqLookupNode->idxEntry,
                         collatorSlot,
                         eqLookupNode->nodeId(),
                         eqLookupNode->children.size(),
                         false /*hasUnwindSrc*/,
                         isForward(eqLookupNode->scanDirection));

    auto [resultSlot, resultStage] = buildLookupResultObject(std::move(foreignStage),
                                                             localRecordSlot,
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
    SbSlot localRecordSlot = localOutputs.get(PlanStageSlots::kResult);

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

    auto [matchedDocumentsSlot, foreignStage] =
        buildLookupStage(_state,
                         eqLookupUnwindNode->lookupStrategy,
                         std::move(localStage),
                         localOutputs,
                         eqLookupUnwindNode->joinFieldLocal,
                         eqLookupUnwindNode->joinFieldForeign,
                         foreignColl,
                         eqLookupUnwindNode->idxEntry,
                         collatorSlot,
                         eqLookupUnwindNode->nodeId(),
                         eqLookupUnwindNode->children.size(),
                         true /*hasUnwindSrc*/,
                         isForward(eqLookupUnwindNode->scanDirection));

    if (eqLookupUnwindNode->lookupStrategy ==
        EqLookupNode::LookupStrategy::kNonExistentForeignCollection) {
        // Build the absorbed $unwind as our parent. We don't need to avoid materializing the lookup
        // result array since it is empty. (It would be much more difficult to detect non-existent
        // foreign collection in DocumentSourceLookup::doOptimizeAt() to avoid ever absorbing the
        // $unwind, as that computation happens later and requires several inputs that are not
        // available at the time of doOptimizeAt().)
        return buildOnlyUnwind(eqLookupUnwindNode->unwindSpec,
                               reqs,
                               eqLookupUnwindNode->nodeId(),
                               foreignStage,
                               localOutputs,
                               localRecordSlot,
                               matchedDocumentsSlot);
    }

    auto [resultSlot, resultStage] = buildLookupResultObject(std::move(foreignStage),
                                                             localRecordSlot,
                                                             matchedDocumentsSlot,
                                                             eqLookupUnwindNode->joinField,
                                                             eqLookupUnwindNode->nodeId(),
                                                             _state,
                                                             eqLookupUnwindNode->shouldProduceBson);

    PlanStageSlots outputs;
    outputs.set(kResult, resultSlot);
    return {std::move(resultStage), std::move(outputs)};
}  // buildEqLookupUnwind

namespace {
PlanStageReqs makeReqsForRightSideOfNestedLoopJoin(
    const QuerySolutionNode* root, std::vector<PlanStageReqs::OwnedSlotName> fieldRequests) {
    auto [leaf, numCollScanNodes] = root->getFirstNodeByType(STAGE_COLLSCAN);
    auto collectionScanNode = dynamic_cast<const CollectionScanNode*>(leaf);
    tassert(10984700,
            "Expected exactly one CollectionScanNode in right side of NestedLoopJoinEmbeddingNode",
            collectionScanNode != nullptr && numCollScanNodes == 1);

    return PlanStageReqs{}
        .setTargetNamespace(collectionScanNode->nss)
        .setResultObj()
        .set(std::move(fieldRequests));
}

/**
 * Generates an expression for evaluating a path that takes the value found at the first path
 * component and returns a single value from evaluating the remaining path components. The
 * expression assumes that there are no arrays along the path, and it will not perform implicit
 * array traversal.
 */
SbExpr generateArrayObliviousPathEvaluation(SbBuilder& b,
                                            const FieldPath& path,
                                            PlanStageSlots& outputs) {
    // Try if a prefix or the entire path is already available in a slot.
    for (size_t i = path.getPathLength(); i > 0; --i) {
        auto prefix = path.getSubpath(i - 1);
        auto slotExpr =
            outputs.getIfExists(std::make_pair(PlanStageSlots::SlotType::kField, prefix));
        if (!slotExpr) {
            slotExpr =
                outputs.getIfExists(std::make_pair(PlanStageSlots::SlotType::kPathExpr, prefix));
        }
        if (slotExpr) {
            SbExpr expr = *slotExpr;

            // NOTE: We're generating an expression that operates on the already evaluated first
            // path component(s), which is why we start this loop from the current index.
            for (; i < path.getPathLength(); ++i) {
                expr = b.makeFunction(
                    "getField"_sd, std::move(expr), b.makeStrConstant(path.getFieldName(i)));
            }

            return expr;
        }
    }
    MONGO_UNREACHABLE_TASSERT(11158604);
}

std::pair<SbStage, PlanStageSlots> generateJoinResult(const BinaryJoinEmbeddingNode* node,
                                                      const PlanStageReqs& reqs,
                                                      SbStage stage,
                                                      const PlanStageSlots& leftOutputs,
                                                      const PlanStageSlots& rightOutputs,
                                                      const FieldEffects& fieldEffect,
                                                      StageBuilderState& state) {
    auto rootDocumentSlot = [&]() {
        if (node->leftEmbeddingField && !node->rightEmbeddingField) {
            return rightOutputs.get(SlotBasedStageBuilder::kResult);
        } else if (!node->leftEmbeddingField && node->rightEmbeddingField) {
            return leftOutputs.get(SlotBasedStageBuilder::kResult);
        } else if (node->leftEmbeddingField && node->rightEmbeddingField) {
            // Both sides are embedded, return a Nothing result object.
            return SbSlot{state.getNothingSlot()};
        } else {
            // In a bushy plan we could be joining two streams that don't contain the base
            // collection.
            if (leftOutputs.get(SlotBasedStageBuilder::kResult).getId() == state.getNothingSlot() &&
                rightOutputs.get(SlotBasedStageBuilder::kResult).getId() ==
                    state.getNothingSlot()) {
                return SbSlot{state.getNothingSlot()};
            }
            // Both sides are not embedded, the result object is the one that is not Nothing.
            if (leftOutputs.get(SlotBasedStageBuilder::kResult).getId() != state.getNothingSlot() &&
                rightOutputs.get(SlotBasedStageBuilder::kResult).getId() ==
                    state.getNothingSlot()) {
                return leftOutputs.get(SlotBasedStageBuilder::kResult);
            }
            if (leftOutputs.get(SlotBasedStageBuilder::kResult).getId() == state.getNothingSlot() &&
                rightOutputs.get(SlotBasedStageBuilder::kResult).getId() !=
                    state.getNothingSlot()) {
                return rightOutputs.get(SlotBasedStageBuilder::kResult);
            }
            tasserted(11158603, "Cannot join two streams having two different result objects");
        }
    }();

    if (reqs.hasResultInfo()) {
        PlanStageSlots outputs;
        outputs.setResultInfoBaseObj(rootDocumentSlot);
        // Each side that is not embedded should project all its outputs. If it is embedded, it
        // should project the embedding field. We also propagate the slot we have precomputed even
        // when we are embedded, so that we avoid re-fetching them from the result object.
        auto projectFields = [&outputs](const boost::optional<FieldPath>& embedding,
                                        const PlanStageSlots& childOutputs) {
            if (!embedding) {
                for (auto projectedSlot : childOutputs.getAllNameSlotPairsInOrder()) {
                    outputs.set(projectedSlot.first, projectedSlot.second);
                }
            } else {
                for (auto projectedSlot : childOutputs.getAllNameSlotPairsInOrder()) {
                    if (projectedSlot.first.first == PlanStageSlots::SlotType::kField) {
                        auto key = std::make_pair(
                            PlanStageSlots::SlotType::kPathExpr,
                            embedding->concat(projectedSlot.first.second).fullPath());
                        outputs.set(key, projectedSlot.second);
                    }
                }
                outputs.set(std::make_pair(PlanStageSlots::SlotType::kField, embedding->fullPath()),
                            childOutputs.get(SlotBasedStageBuilder::kResult));
            }
        };
        projectFields(node->leftEmbeddingField, leftOutputs);
        projectFields(node->rightEmbeddingField, rightOutputs);

        outputs.addEffectsToResultInfo(state, reqs, fieldEffect);
        return {std::move(stage), std::move(outputs)};
    }

    // Finally, build the projection that constructs a single join result from a pair of documents
    // by embedding one into the other.
    std::vector<ProjectNode> nodes;
    std::vector<std::string> paths;

    if (node->rightEmbeddingField) {
        paths.emplace_back(node->rightEmbeddingField->fullPath());
        nodes.emplace_back(rightOutputs.get(SlotBasedStageBuilder::kResult));
    }
    if (node->leftEmbeddingField) {
        paths.emplace_back(node->leftEmbeddingField->fullPath());
        nodes.emplace_back(leftOutputs.get(SlotBasedStageBuilder::kResult));
    }
    for (const auto& field : fieldEffect.getFieldList()) {
        if ((!node->rightEmbeddingField || field != node->rightEmbeddingField->fullPath()) &&
            (!node->leftEmbeddingField || field != node->leftEmbeddingField->fullPath())) {
            paths.emplace_back(field);
            // TODO: SERVER-113230 in case of conflict, we give priority to the left side. Depending
            // on the join graph, an embedding having the same name of a top-level field in the base
            // collection could fail to overwrite it.
            auto key = std::make_pair(SlotBasedStageBuilder::kField, field);
            nodes.emplace_back(leftOutputs.has(key) ? leftOutputs.get(key) : rightOutputs.get(key));
        }
    }
    auto [resultSlot, embedStage] = buildLookupResultObject(std::move(stage),
                                                            rootDocumentSlot,
                                                            std::move(nodes),
                                                            std::move(paths),
                                                            node->nodeId(),
                                                            state,
                                                            true /* shouldProduceBson */);

    PlanStageSlots outputs;
    outputs.setResultObj(resultSlot);
    return {std::move(embedStage), std::move(outputs)};
}

/**
 * Collect the top-level fields needed from each side of the join to evaluate the join
 * predicates.
 */
std::pair<std::vector<PlanStageReqs::OwnedSlotName>, std::vector<PlanStageReqs::OwnedSlotName>>
collectRequestedFields(const BinaryJoinEmbeddingNode* node,
                       const PlanStageReqs& reqs,
                       const QsnAnalysis& qsnAnalysis) {
    StringSet leftTopLevelFields, rightTopLevelFields, leftPaths, rightPaths;
    for (const auto& predicate : node->joinPredicates) {
        tassert(10984702, "Empty path in join predicate", predicate.leftField.getPathLength() > 0);
        // Request the longest prefix that is listed in the output of this side; if it cannot be
        // found, request the first part of the path.
        auto& leftChildFieldEffect = qsnAnalysis.getQsnInfo(node->children[0]).effects;
        size_t i = 0;
        if (leftChildFieldEffect) {
            for (i = predicate.leftField.getPathLength() - 1; i > 0; i--) {
                auto prefix = predicate.leftField.getSubpath(i - 1);
                if (leftChildFieldEffect->isAllowedField(prefix)) {
                    leftTopLevelFields.insert(std::string(prefix));
                    break;
                }
            }
        }
        if (i == 0) {
            leftTopLevelFields.insert(std::string(predicate.leftField.front()));
        }
        // Add an optional request for the entire path.
        if (predicate.leftField.getPathLength() > 1) {
            leftPaths.insert(predicate.leftField.fullPath());
        }

        tassert(10984703, "Empty path in join predicate", predicate.rightField.getPathLength() > 0);
        // Request the longest prefix that is listed in the output of this side; if it cannot be
        // found, request the first part of the path.
        auto& rightChildFieldEffect = qsnAnalysis.getQsnInfo(node->children[1]).effects;
        i = 0;
        if (rightChildFieldEffect) {
            for (i = predicate.rightField.getPathLength() - 1; i > 0; i--) {
                auto prefix = predicate.rightField.getSubpath(i - 1);
                if (rightChildFieldEffect->isAllowedField(prefix)) {
                    rightTopLevelFields.insert(std::string(prefix));
                    break;
                }
            }
        }
        if (i == 0) {
            rightTopLevelFields.insert(std::string(predicate.rightField.front()));
        }
        // Add an optional request for the entire path.
        if (predicate.rightField.getPathLength() > 1) {
            rightPaths.insert(predicate.rightField.fullPath());
        }
    }

    auto& fieldEffect = qsnAnalysis.getQsnInfo(node).effects;
    tassert(11158600, "Expected field effect set to be computed", fieldEffect);

    auto forwardFields = [&qsnAnalysis, &reqs, &fieldEffect](
                             StringSet& targetSet,
                             const std::unique_ptr<QuerySolutionNode>& childNode,
                             const std::unique_ptr<QuerySolutionNode>& otherChildNode,
                             const boost::optional<FieldPath>& thisEmbedding,
                             const boost::optional<FieldPath>& otherEmbedding) {
        auto& childFieldEffect = qsnAnalysis.getQsnInfo(childNode).effects;
        auto& otherChildFieldEffect = qsnAnalysis.getQsnInfo(otherChildNode).effects;
        bool childHoldsMainCollection = false;
        std::list<const QuerySolutionNode*> children = {childNode.get()};
        while (!children.empty()) {
            auto node = children.front();
            children.pop_front();
            auto joinNode = dynamic_cast<const BinaryJoinEmbeddingNode*>(node);
            if (joinNode) {
                if (!joinNode->leftEmbeddingField) {
                    children.push_back(joinNode->children[0].get());
                }
                if (!joinNode->rightEmbeddingField) {
                    children.push_back(joinNode->children[1].get());
                }
            } else {
                childHoldsMainCollection = true;
            }
        }
        // If this side is not embedded, forward all the requested fields to it, provided that:
        // 1) this side if the main collection or the field is an explicit output of this side
        // 2) the field is not listed as one of the outputs of the other side
        // 3) the field is not the embedding of the other side
        if (!thisEmbedding) {
            for (auto& field : reqs.getFields()) {
                if ((childHoldsMainCollection ||
                     (childFieldEffect && childFieldEffect->isAllowedField(field))) &&
                    (!otherChildFieldEffect || !otherChildFieldEffect->isAllowedField(field)) &&
                    (!otherEmbedding || field != *otherEmbedding)) {
                    targetSet.insert(field);
                }
            }
        }
        // Request also those fields that we have to output and that come from this side.
        if (childFieldEffect) {
            for (const auto& field : childFieldEffect->getFieldList()) {
                if (childFieldEffect->isAllowedField(field) && fieldEffect->isAllowedField(field)) {
                    targetSet.insert(field);
                }
            }
        }
    };
    forwardFields(leftTopLevelFields,
                  node->children[0],
                  node->children[1],
                  node->leftEmbeddingField,
                  node->rightEmbeddingField);
    forwardFields(rightTopLevelFields,
                  node->children[1],
                  node->children[0],
                  node->rightEmbeddingField,
                  node->leftEmbeddingField);

    std::vector<PlanStageReqs::OwnedSlotName> leftRequests, rightRequests;
    for (auto& field : leftTopLevelFields) {
        leftRequests.emplace_back(std::make_pair(PlanStageReqs::kField, field));
    }
    for (auto& field : leftPaths) {
        leftRequests.emplace_back(std::make_pair(PlanStageReqs::kPathExpr, field));
    }
    for (auto& field : rightTopLevelFields) {
        rightRequests.emplace_back(std::make_pair(PlanStageReqs::kField, field));
    }
    for (auto& field : rightPaths) {
        rightRequests.emplace_back(std::make_pair(PlanStageReqs::kPathExpr, field));
    }
    return std::make_pair(std::move(leftRequests), std::move(rightRequests));
}
}  // namespace

/**
 * Build an equijoin operation according to the input STAGE_NESTED_LOOP_JOIN_EMBEDDING_NODE plan.
 * This style of join is simpler than general-purpose $lookup joins, because it only supports
 * equality predicates with path operands that never implicitly traverse an array.
 */
std::pair<SbStage, PlanStageSlots> SlotBasedStageBuilder::buildNestedLoopJoinEmbeddingNode(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    tassert(10984701,
            "buildNestedLoopJoinEmbeddingNode() does not support kSortKey",
            !reqs.hasSortKeys());

    SbBuilder b(_state, root->nodeId());

    auto nestedLoopJoinEmbeddingNode = static_cast<const NestedLoopJoinEmbeddingNode*>(root);

    auto [leftRequests, rightRequests] =
        collectRequestedFields(nestedLoopJoinEmbeddingNode, reqs, _qsnAnalysis);

    auto& fieldEffect = _qsnAnalysis.getQsnInfo(root).effects;
    tassert(11158601, "Expected field effect set to be computed", fieldEffect);

    // Recursively build the executable plan for each side of the join.
    PlanStageReqs leftChildReqs =
        PlanStageReqs{}
            .setTargetNamespace(reqs.getTargetNamespace())
            .setResultInfo(FieldSet::makeOpenSet(std::vector<std::string>{}), FieldEffects())
            .set(std::move(leftRequests));
    auto [leftStage, leftOutputs] =
        build(nestedLoopJoinEmbeddingNode->children[0].get(), leftChildReqs);

    PlanStageReqs rightChildReqs = makeReqsForRightSideOfNestedLoopJoin(
        nestedLoopJoinEmbeddingNode->children[1].get(), std::move(rightRequests));
    auto [rightStage, rightOutputs] =
        build(nestedLoopJoinEmbeddingNode->children[1].get(), rightChildReqs);

    // Build the equality predicates for the join condition and the 'outerProjects' list of slots
    // from the outer side of the join that must be available to join predicate and/or the parent
    // stage. All slots from the inner side of the join are automatically available.
    SbExpr::Vector equalityPredicates;
    SbSlotVector outerProjects;
    sbe::value::SlotSet
        outerProjectsSet;  // Used to avoid duplicates in the 'outerProjections' list.
    if (leftOutputs.has(kResult)) {
        auto resultObj = leftOutputs.get(kResult);
        outerProjects.emplace_back(resultObj);
        outerProjectsSet.insert(resultObj.getId());
    }

    // ensure that requested fields coming from the left side are propagated (the right side is
    // automatically exposed).
    for (const auto& requestedFields : {fieldEffect->getFieldList(), reqs.getFields()}) {
        for (const auto& field : requestedFields) {
            if (auto slot = leftOutputs.getIfExists(std::make_pair(kField, field)); slot) {
                if (auto [_, inserted] = outerProjectsSet.insert(slot->getId()); inserted) {
                    outerProjects.emplace_back(slot->getId());
                }
            }
        }
    }

    equalityPredicates.reserve(nestedLoopJoinEmbeddingNode->joinPredicates.size());
    for (const auto& predicate : nestedLoopJoinEmbeddingNode->joinPredicates) {
        // Make sure that the join predicate has access to any slot needed to evaluate the left
        // path.
        // NOTE: This assumes that calling PlanStageSlots::get() for a top-level field will always
        // return either an SbSlot or an expression that depends on only the ResultObj slot.
        for (size_t i = 0; i < predicate.leftField.getPathLength(); ++i) {
            auto prefix = predicate.leftField.getSubpath(i);
            auto slot = leftOutputs.getIfExists(std::make_pair(PlanStageSlots::kField, prefix));
            if (slot) {
                if (auto [_, inserted] = outerProjectsSet.insert(slot->getId()); inserted) {
                    outerProjects.emplace_back(slot->getId());
                }
            }
            slot = leftOutputs.getIfExists(std::make_pair(PlanStageSlots::kPathExpr, prefix));
            if (slot) {
                if (auto [_, inserted] = outerProjectsSet.insert(slot->getId()); inserted) {
                    outerProjects.emplace_back(slot->getId());
                }
            }
        }

        tassert(10984704,
                "Unknown operation in join predicate",
                predicate.op == QSNJoinPredicate::ComparisonOp::Eq);

        // Generate an expression for one predicate, which evaluates a path on each document and
        // compares the resulting values. Any path that fails to evaluate, because of a missing or
        // non-object path component, gets treated as if it evaluated to a null value for the
        // purposes of this comparison. This behavior matches MQL localField/foreignField $lookup
        // semantics.
        equalityPredicates.emplace_back(b.makeBinaryOp(
            abt::Operations::Eq,
            b.makeFillEmptyNull(
                generateArrayObliviousPathEvaluation(b, predicate.leftField, leftOutputs)),
            b.makeFillEmptyNull(
                generateArrayObliviousPathEvaluation(b, predicate.rightField, rightOutputs))));
    }

    // Build the LoopJoin stage that implements the nested loop join, including the equality test.
    auto loopJoinStage =
        b.makeLoopJoin(std::move(leftStage),
                       std::move(rightStage),
                       outerProjects,
                       {} /* outerCorrelated */,
                       {} /* innerProjects */,
                       b.makeBooleanOpTree(abt::Operations::And, std::move(equalityPredicates)));

    return generateJoinResult(nestedLoopJoinEmbeddingNode,
                              reqs,
                              std::move(loopJoinStage),
                              leftOutputs,
                              rightOutputs,
                              *fieldEffect,
                              _state);
}

/**
 * Build an equijoin operation according to the input STAGE_HASH_JOIN_EMBEDDING_NODE plan.
 * This style of join is simpler than general-purpose $lookup joins, because it only supports
 * equality predicates with path operands that never implicitly traverse an array.
 */
std::pair<SbStage, PlanStageSlots> SlotBasedStageBuilder::buildHashJoinEmbeddingNode(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    tassert(
        11122101, "buildHashJoinEmbeddingNode() does not support kSortKey", !reqs.hasSortKeys());

    SbBuilder b(_state, root->nodeId());

    auto hashJoinEmbeddingNode = static_cast<const HashJoinEmbeddingNode*>(root);

    auto [leftRequests, rightRequests] =
        collectRequestedFields(hashJoinEmbeddingNode, reqs, _qsnAnalysis);

    auto& fieldEffect = _qsnAnalysis.getQsnInfo(root).effects;
    tassert(11158602, "Expected field effect set to be computed", fieldEffect);

    // Recursively build the executable plan for each side of the join.
    PlanStageReqs leftChildReqs =
        PlanStageReqs{}
            .setTargetNamespace(reqs.getTargetNamespace())
            .setResultInfo(FieldSet::makeOpenSet(std::vector<std::string>{}), FieldEffects())
            .set(std::move(leftRequests));
    auto [leftStage, leftOutputs] = build(hashJoinEmbeddingNode->children[0].get(), leftChildReqs);

    PlanStageReqs rightChildReqs = makeReqsForRightSideOfNestedLoopJoin(
        hashJoinEmbeddingNode->children[1].get(), std::move(rightRequests));
    auto [rightStage, rightOutputs] =
        build(hashJoinEmbeddingNode->children[1].get(), rightChildReqs);

    SbExprOptSlotVector leftPrj, rightPrj;
    for (const auto& predicate : hashJoinEmbeddingNode->joinPredicates) {
        tassert(11122104,
                "Unknown operation in join predicate",
                predicate.op == QSNJoinPredicate::ComparisonOp::Eq);

        // Create an expression for each side of the predicate, and add it to a $project stage to be
        // placed on top of the source stages. Any path that fails to evaluate, because of a missing
        // or non-object path component, gets treated as if it evaluated to a null value for the
        // purposes of this comparison. This behavior matches MQL localField/foreignField $lookup
        // semantics.
        leftPrj.emplace_back(b.makeFillEmptyNull(generateArrayObliviousPathEvaluation(
                                 b, predicate.leftField, leftOutputs)),
                             boost::none);
        rightPrj.emplace_back(b.makeFillEmptyNull(generateArrayObliviousPathEvaluation(
                                  b, predicate.rightField, rightOutputs)),
                              boost::none);
    }
    auto [leftPrjStage, leftPrjOutputs] = b.makeProject(std::move(leftStage), std::move(leftPrj));
    auto [rightPrjStage, rightPrjOutputs] =
        b.makeProject(std::move(rightStage), std::move(rightPrj));

    // Propagate all the slots created by the children.
    SbSlotVector leftProjectSlots, rightProjectSlots;
    for (auto& produce : leftOutputs.getAllNameSlotPairsInOrder()) {
        leftProjectSlots.emplace_back(produce.second);
    }
    for (auto& produce : rightOutputs.getAllNameSlotPairsInOrder()) {
        rightProjectSlots.emplace_back(produce.second);
    }

    // Build the HashJoin stage that implements the hash join.
    auto hashJoinStage = b.makeHashJoin(std::move(rightPrjStage),
                                        std::move(leftPrjStage),
                                        rightPrjOutputs,
                                        rightProjectSlots,
                                        leftPrjOutputs,
                                        leftProjectSlots,
                                        boost::none);

    return generateJoinResult(hashJoinEmbeddingNode,
                              reqs,
                              std::move(hashJoinStage),
                              leftOutputs,
                              rightOutputs,
                              *fieldEffect,
                              _state);
}

}  // namespace mongo::stage_builder
