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


#include "mongo/platform/basic.h"

#include "mongo/db/query/sbe_stage_builder.h"

#include <fmt/format.h>

#include "mongo/db/curop.h"
#include "mongo/db/exec/sbe/stages/branch.h"
#include "mongo/db/exec/sbe/stages/hash_agg.h"
#include "mongo/db/exec/sbe/stages/hash_lookup.h"
#include "mongo/db/exec/sbe/stages/ix_scan.h"
#include "mongo/db/exec/sbe/stages/limit_skip.h"
#include "mongo/db/exec/sbe/stages/loop_join.h"
#include "mongo/db/exec/sbe/stages/scan.h"
#include "mongo/db/exec/sbe/stages/union.h"
#include "mongo/db/exec/sbe/stages/unique.h"
#include "mongo/db/exec/sbe/stages/unwind.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/query/sbe_stage_builder_coll_scan.h"
#include "mongo/db/query/sbe_stage_builder_expression.h"
#include "mongo/db/query/sbe_stage_builder_filter.h"
#include "mongo/db/query/sbe_stage_builder_helpers.h"
#include "mongo/db/query/sbe_stage_builder_index_scan.h"
#include "mongo/db/query/sbe_stage_builder_projection.h"
#include "mongo/db/query/util/make_data_structure.h"
#include "mongo/logv2/log.h"

#include "mongo/db/query/sbe_stage_builder_filter.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo::stage_builder {
/**
 * Helpers for building $lookup.
 */
namespace {
using namespace sbe;
using namespace sbe::value;

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
 *      {a: {b: [1, null, 2]}
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
 *      {a: {b: [1, null, 2]}
 *      {a: [{b: [1, null, 2]}, {b: 3}]}
 *
 * 2. there is a missing terminal, such that the last value on the resolved path to this terminal
 *    is not a scalar inside array. For example, if foreignField:"a.b.c", the following records
 *    would match:
 *      {a: {b: {no_c: 1}}} // a.b._ last value {no_c: 1} isn't a scalar
 *      {a: {b: 1}} // a.b._ last value 1 is a scalar but it's not inside an array
 *      {a: [{b: {no_c: 1}}, {b: {c: 2}}]} // a.0.b._ last value {no_c: 1} isn't a scalar
 *      {a: [{b: [{c: 1}, {c: 2}]}, {b: [{c: 3}, {no_c: 4}]]} // a.1.b.1._ last value {no_c: 4}
 *      {a: 1} // a._ last value 1 is a scalar but it's not inside an array
 *      {no_a: 1} // _ last value {no_a: 1} isn't a scalar
 *
 *    but these records won't match:
 *      {a: [1, 2]} // a.0._ and a.1._ end in scalar values inside array
 *      {a: {b: [1, 2]}} // a.b.0._ and a.b.1._ end in scalar values inside array
 *      {a: [{b: [1, 2]}, 3]} // a.0.b.0._, a.0.b.1._ and a.1.b._ end in scalar values inside arrays
 */

enum class JoinSide { Local = 0, Foreign = 1 };

// Creates stages for traversing path 'fp' in the record from 'inputSlot' that implement MQL
// semantics for local collections. The semantics never treat terminal arrays as whole values and
// match to null per "Matching local records to null" above. Returns one key value at a time.
// For example, if the record in the 'inputSlot' is:
//     {a: [{b:[1,[2,3]]}, {b:4}, {b:1}, {b:2}]},
// the returned values for path "a.b" will be streamed as: 1, [2,3], 4, 1, 2.
// Empty arrays and missing are skipped, that is, if the record in the 'inputSlot' is:
//     {a: [{b:1}, {b:[]}, {no_b:42}, {b:2}]},
// the returned values for path "a.b" will be streamed as: 1, 2.
std::pair<SlotId /* keyValueSlot */, std::unique_ptr<sbe::PlanStage>> buildLocalKeysStream(
    SlotId inputSlot,
    const FieldPath& fp,
    const PlanNodeId nodeId,
    SlotIdGenerator& slotIdGenerator) {
    const FieldIndex numParts = fp.getPathLength();

    std::unique_ptr<sbe::PlanStage> currentStage = makeLimitCoScanTree(nodeId, 1);
    SlotId keyValueSlot = inputSlot;
    for (size_t i = 0; i < numParts; i++) {
        const StringData fieldName = fp.getFieldName(i);

        SlotId getFieldSlot = slotIdGenerator.generate();
        currentStage = makeProjectStage(
            std::move(currentStage),
            nodeId,
            getFieldSlot,
            makeFunction("getField"_sd, makeVariable(keyValueSlot), makeConstant(fieldName)));

        SlotId unwindOutputSlot = slotIdGenerator.generate();
        currentStage = makeS<UnwindStage>(std::move(currentStage) /* child stage */,
                                          getFieldSlot,
                                          unwindOutputSlot,
                                          slotIdGenerator.generate() /* outIndex */,
                                          true /* preserveNullAndEmptyArrays */,
                                          nodeId);
        keyValueSlot = unwindOutputSlot;
    }
    return {keyValueSlot, std::move(currentStage)};
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
std::pair<SlotId /* keyValueSlot */, std::unique_ptr<sbe::PlanStage>> buildForeignKeysStream(
    SlotId inputSlot,
    const FieldPath& fp,
    const PlanNodeId nodeId,
    SlotIdGenerator& slotIdGenerator) {
    const FieldIndex numParts = fp.getPathLength();

    SlotId keyValueSlot = inputSlot;
    SlotId prevKeyValueSlot = inputSlot;
    std::unique_ptr<sbe::PlanStage> currentStage = makeLimitCoScanTree(nodeId, 1);

    for (size_t i = 0; i < numParts; i++) {
        const StringData fieldName = fp.getFieldName(i);

        std::unique_ptr<EExpression> getFieldFromObject;
        if (i == 0) {
            // 'inputSlot' must contain a document and, by definition, it's not inside an array, so
            // can get field unconditionally.
            getFieldFromObject = makeFillEmptyNull(
                makeFunction("getField"_sd, makeVariable(keyValueSlot), makeConstant(fieldName)));
        } else {
            // Don't get field from scalars inside arrays (it would fail but we also don't want to
            // fill with "null" in this case to match the MQL semantics described above.)
            std::unique_ptr<EExpression> shouldGetField =
                makeBinaryOp(EPrimBinary::logicOr,
                             makeFunction("isObject", makeVariable(keyValueSlot)),
                             makeUnaryOp(EPrimUnary::logicNot,
                                         makeFunction("isArray", makeVariable(prevKeyValueSlot))));
            getFieldFromObject =
                makeE<EIf>(std::move(shouldGetField),
                           makeFillEmptyNull(makeFunction(
                               "getField"_sd, makeVariable(keyValueSlot), makeConstant(fieldName))),
                           makeConstant(TypeTags::Nothing, 0));
        }

        SlotId getFieldSlot = slotIdGenerator.generate();
        currentStage = makeProjectStage(
            move(currentStage), nodeId, getFieldSlot, std::move(getFieldFromObject));
        keyValueSlot = getFieldSlot;

        // For the terminal array we will do the extra work of adding the array itself to the stream
        // (see below) but for the non-termial path components we only need to unwind array
        // elements.
        if (i + 1 < numParts) {
            SlotId unwindOutputSlot = slotIdGenerator.generate();
            currentStage = makeS<UnwindStage>(std::move(currentStage) /* child stage */,
                                              keyValueSlot,
                                              unwindOutputSlot,
                                              slotIdGenerator.generate() /* outIndex */,
                                              true /* preserveNullAndEmptyArrays */,
                                              nodeId);
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

    SlotId terminalUnwindOutputSlot = slotIdGenerator.generate();
    std::unique_ptr<sbe::PlanStage> terminalUnwind =
        makeS<UnwindStage>(makeLimitCoScanTree(nodeId, 1) /* child stage */,
                           keyValueSlot,
                           terminalUnwindOutputSlot,
                           slotIdGenerator.generate() /* outIndex */,
                           true /* preserveNullAndEmptyArrays */,
                           nodeId);

    SlotId unionOutputSlot = slotIdGenerator.generate();
    sbe::PlanStage::Vector terminalStagesToUnion;
    terminalStagesToUnion.push_back(std::move(terminalUnwind));
    terminalStagesToUnion.emplace_back(makeLimitCoScanTree(nodeId, 1));

    std::unique_ptr<sbe::PlanStage> unionStage =
        makeS<UnionStage>(std::move(terminalStagesToUnion),
                          std::vector{makeSV(terminalUnwindOutputSlot), makeSV(keyValueSlot)},
                          makeSV(unionOutputSlot),
                          nodeId);

    SlotId maybeUnionOutputSlot = slotIdGenerator.generate();
    unionStage = makeS<BranchStage>(std::move(unionStage),
                                    makeLimitCoScanTree(nodeId, 1),
                                    makeFunction("isArray", makeVariable(keyValueSlot)),
                                    SlotVector{unionOutputSlot},
                                    SlotVector{keyValueSlot},
                                    SlotVector{maybeUnionOutputSlot},
                                    nodeId);

    currentStage = makeS<LoopJoinStage>(std::move(currentStage),
                                        std::move(unionStage),
                                        makeSV() /* outerProjects */,
                                        makeSV(keyValueSlot) /* outerCorrelated */,
                                        nullptr /* predicate */,
                                        nodeId);
    keyValueSlot = maybeUnionOutputSlot;

    return {keyValueSlot, std::move(currentStage)};
}

// Creates stages for traversing path 'fp' in the record from 'inputSlot'. Puts the set of key
// values into 'keyValuesSetSlot. For example, if the record in the 'inputSlot' is:
//     {a: [{b:[1,[2,3]]}, {b:4}, {b:1}, {b:2}]},
// the returned slot will contain for path "a.b" a set of {1, 2, 4, [2,3]}.
// If the stream produces no values, that is, would result in an empty set, the empty set is
// replaced with a set that contains a single 'null' value, so that it matches MQL semantics when
// empty arrays and all missing are matched to 'null'.
std::pair<SlotId /* keyValuesSetSlot */, std::unique_ptr<sbe::PlanStage>> buildKeySet(
    JoinSide joinSide,
    std::unique_ptr<sbe::PlanStage> inputStage,
    SlotId recordSlot,
    const FieldPath& fp,
    const PlanNodeId nodeId,
    SlotIdGenerator& slotIdGenerator,
    bool allowDiskUse) {
    // Create the branch to stream individual key values from every terminal of the path.
    auto [keyValueSlot, keyValuesStage] = (joinSide == JoinSide::Local)
        ? buildLocalKeysStream(recordSlot, fp, nodeId, slotIdGenerator)
        : buildForeignKeysStream(recordSlot, fp, nodeId, slotIdGenerator);

    // Re-pack the individual key values into a set. We don't cap "addToSet" here because its size
    // is bounded by the size of the record.
    SlotId keyValuesSetSlot = slotIdGenerator.generate();
    EvalStage packedKeyValuesStage = makeHashAgg(
        EvalStage{std::move(keyValuesStage), SlotVector{}},
        makeSV(), /* groupBy slots - "none" means creating a single group */
        makeEM(keyValuesSetSlot, makeFunction("addToSet"_sd, makeVariable(keyValueSlot))),
        boost::none /* we group _all_ key values into a single set, so collator is irrelevant */,
        allowDiskUse,
        nodeId);

    // The set in 'keyValuesSetSlot' might end up empty if the localField contained only missing and
    // empty arrays (e.g. path "a.b" in {a: [{no_b:1}, {b:[]}]}). The semantics of MQL for local
    // keys require these cases to match to 'null', so we replace the empty set with a constant set
    // that contains a single 'null' value. The set of foreign key values also can be empty but it
    // should produce no matches so we leave it empty.
    if (joinSide == JoinSide::Local) {
        auto [arrayWithNullTag, arrayWithNullVal] = makeNewArray();
        std::unique_ptr<EExpression> arrayWithNull =
            makeConstant(arrayWithNullTag, arrayWithNullVal);
        value::Array* arrayWithNullView = getArrayView(arrayWithNullVal);
        arrayWithNullView->push_back(TypeTags::Null, 0);

        std::unique_ptr<EExpression> isNonEmptySetExpr =
            makeBinaryOp(EPrimBinary::greater,
                         makeFunction("getArraySize", makeVariable(keyValuesSetSlot)),
                         makeConstant(TypeTags::NumberInt32, 0));

        SlotId nonEmptySetSlot = slotIdGenerator.generate();
        packedKeyValuesStage = makeProject(std::move(packedKeyValuesStage),
                                           nodeId,
                                           nonEmptySetSlot,
                                           makeE<EIf>(std::move(isNonEmptySetExpr),
                                                      makeVariable(keyValuesSetSlot),
                                                      std::move(arrayWithNull)));
        keyValuesSetSlot = nonEmptySetSlot;
    }

    // Attach the set of key values to the original local record.
    std::unique_ptr<sbe::PlanStage> nljLocalWithKeyValuesSet =
        makeS<LoopJoinStage>(std::move(inputStage),
                             packedKeyValuesStage.extractStage(nodeId),
                             makeSV(recordSlot) /* outerProjects */,
                             makeSV(recordSlot) /* outerCorrelated */,
                             nullptr /* predicate */,
                             nodeId);

    return {keyValuesSetSlot, std::move(nljLocalWithKeyValuesSet)};
}

// Creates stages for grouping matched foreign records into an array. If there's no match, the
// stages return an empty array instead.
std::pair<SlotId /* resultSlot */, std::unique_ptr<sbe::PlanStage>> buildForeignMatchedArray(
    EvalStage innerBranch,
    SlotId foreignRecordSlot,
    const PlanNodeId nodeId,
    SlotIdGenerator& slotIdGenerator,
    bool allowDiskUse) {
    // $lookup's aggregates the matching records into an array. We currently don't have a stage
    // that could do this grouping _after_ Nlj, so we achieve it by having a hash_agg inside the
    // inner branch that aggregates all matched records into a single accumulator. When there
    // are no matches, return an empty array.
    const int sizeCap = internalLookupStageIntermediateDocumentMaxSizeBytes.load();
    SlotId accumulatorSlot = slotIdGenerator.generate();
    innerBranch = makeHashAgg(
        std::move(innerBranch),
        makeSV(), /* groupBy slots */
        makeEM(accumulatorSlot,
               makeFunction("addToArrayCapped"_sd,
                            makeVariable(foreignRecordSlot),
                            makeConstant(TypeTags::NumberInt32, sizeCap))),
        {} /* collatorSlot, no collation here because we want to return all matches "as is" */,
        allowDiskUse,
        nodeId);

    // 'accumulatorSlot' is either Nothing or contains an array of size two, where the front element
    // is the array of matched records and the back element is their cumulative size (in bytes).
    SlotId matchedRecordsSlot = slotIdGenerator.generate();
    innerBranch =
        makeProject(std::move(innerBranch),
                    nodeId,
                    matchedRecordsSlot,
                    makeFunction("getElement",
                                 makeVariable(accumulatorSlot),
                                 makeConstant(sbe::value::TypeTags::NumberInt32,
                                              static_cast<int>(vm::AggArrayWithSize::kValues))));


    // $lookup is an _outer_ left join that returns an empty array for "as" field rather than
    // dropping the unmatched local records. The branch that accumulates the matched records into an
    // array returns either 1 or 0 results, so to return an empty array for no-matches case we union
    // this branch with a const scan that produces an empty array but limit it to 1, so if the given
    // branch does produce a record, only that record is returned.
    auto [emptyArrayTag, emptyArrayVal] = makeNewArray();
    // Immediately take ownership of the new array (we could use a ValueGuard here but we'll
    // need the constant below anyway).
    std::unique_ptr<EExpression> emptyArrayConst = makeConstant(emptyArrayTag, emptyArrayVal);

    SlotId emptyArraySlot = slotIdGenerator.generate();
    std::unique_ptr<sbe::PlanStage> emptyArrayStage = makeProjectStage(
        makeLimitCoScanTree(nodeId, 1), nodeId, emptyArraySlot, std::move(emptyArrayConst));

    SlotId unionOutputSlot = slotIdGenerator.generate();
    EvalStage unionStage =
        makeUnion(makeVector(EvalStage{innerBranch.extractStage(nodeId), SlotVector{}},
                             EvalStage{std::move(emptyArrayStage), SlotVector{}}),
                  {makeSV(matchedRecordsSlot), makeSV(emptyArraySlot)} /* inputs */,
                  makeSV(unionOutputSlot),
                  nodeId);

    return std::make_pair(
        unionOutputSlot,
        makeLimitSkip(std::move(unionStage), nodeId, 1 /* limit */).extractStage(nodeId));
}

// Creates stages that extract key values from the given foreign record, compares them to the local
// key values and groups the matching records into an array.
std::pair<SlotId /* matched docs */, std::unique_ptr<sbe::PlanStage>> buildForeignMatches(
    SlotId localKeysSetSlot,
    std::unique_ptr<sbe::PlanStage> foreignStage,
    SlotId foreignRecordSlot,
    const FieldPath& foreignFieldName,
    boost::optional<SlotId> collatorSlot,
    const PlanNodeId nodeId,
    SlotIdGenerator& slotIdGenerator,
    bool allowDiskUse) {
    // 'buildForeignKeysStream()' does not take a child stage. To connect the extraction of key
    // values it does to the 'foreignStage' we'll use an 'nlj', inner branch of which will do the
    // key value extraction and filtering. It's possible that the same foreign record contains
    // multiple matching values and to avoid duplication of the record we must limit the inner
    // branch to '1'.
    auto [foreignValueSlot, currentRootStage] =
        buildForeignKeysStream(foreignRecordSlot, foreignFieldName, nodeId, slotIdGenerator);

    currentRootStage =
        makeLimitTree(makeS<FilterStage<false /*IsConst*/>>(
                          std::move(currentRootStage),
                          collatorSlot ? makeFunction("collIsMember",
                                                      makeVariable(*collatorSlot),
                                                      makeVariable(foreignValueSlot),
                                                      makeVariable(localKeysSetSlot))
                                       : makeFunction("isMember",
                                                      makeVariable(foreignValueSlot),
                                                      makeVariable(localKeysSetSlot)),
                          nodeId),
                      nodeId,
                      1 /* take the foreign record once, even if multiple key values match */);

    currentRootStage = makeS<LoopJoinStage>(std::move(foreignStage) /* outer */,
                                            std::move(currentRootStage) /* inner */,
                                            makeSV(foreignRecordSlot) /* outerProjects */,
                                            makeSV(foreignRecordSlot) /* outerCorrelated */,
                                            nullptr,
                                            nodeId);

    // Group the matched foreign documents into a list.
    // It creates a union stage internally so that when there's no matching foreign records, an
    // empty array will be returned.
    return buildForeignMatchedArray(
        EvalStage{std::move(currentRootStage), makeSV(foreignRecordSlot)},
        foreignRecordSlot,
        nodeId,
        slotIdGenerator,
        allowDiskUse);
}

std::pair<SlotId /* matched docs */, std::unique_ptr<sbe::PlanStage>> buildNljLookupStage(
    StageBuilderState& state,
    std::unique_ptr<sbe::PlanStage> localStage,
    SlotId localRecordSlot,
    const FieldPath& localFieldName,
    std::unique_ptr<sbe::PlanStage> foreignStage,
    SlotId foreignRecordSlot,
    StringData foreignFieldName,
    boost::optional<SlotId> collatorSlot,
    const PlanNodeId nodeId,
    SlotIdGenerator& slotIdGenerator) {
    CurOp::get(state.opCtx)->debug().nestedLoopJoin += 1;

    // Build the outer branch that produces the set of local key values.
    auto [localKeySlot, outerRootStage] = buildKeySet(JoinSide::Local,
                                                      std::move(localStage),
                                                      localRecordSlot,
                                                      localFieldName,
                                                      nodeId,
                                                      slotIdGenerator,
                                                      state.allowDiskUse);

    // Build the inner branch that will get the foreign key values, compare them to the local key
    // values and accumulate all matching foreign records into an array that is placed into
    // 'matchedRecordsSlot'.
    auto [matchedRecordsSlot, innerRootStage] = buildForeignMatches(localKeySlot,
                                                                    std::move(foreignStage),
                                                                    foreignRecordSlot,
                                                                    foreignFieldName,
                                                                    collatorSlot,
                                                                    nodeId,
                                                                    slotIdGenerator,
                                                                    state.allowDiskUse);

    // 'innerRootStage' should not participate in trial run tracking as the number of reads that
    // it performs should not influence planning decisions made for 'outerRootStage'.
    innerRootStage->disableTrialRunTracking();

    // Connect the two branches with a nested loop join. For each outer record with a corresponding
    // value in the 'localKeySlot', the inner branch will be executed and will place the result into
    // 'matchedRecordsSlot'.
    std::unique_ptr<sbe::PlanStage> nlj =
        makeS<LoopJoinStage>(std::move(outerRootStage),
                             std::move(innerRootStage),
                             makeSV(localRecordSlot) /* outerProjects */,
                             makeSV(localKeySlot) /* outerCorrelated */,
                             nullptr /* predicate */,
                             nodeId);

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
 *           cfilter {isNull (localValue)}
 *           project [valueForIndexBounds = undefined]
 *           limit 1
 *           coscan
 *           ,
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
 *
 */
std::pair<SlotId, std::unique_ptr<sbe::PlanStage>> buildIndexJoinLookupStage(
    StageBuilderState& state,
    std::unique_ptr<sbe::PlanStage> localStage,
    SlotId localRecordSlot,
    const FieldPath& localFieldName,
    const FieldPath& foreignFieldName,
    const CollectionPtr& foreignColl,
    const IndexEntry& index,
    StringMap<const IndexAccessMethod*>& iamMap,
    PlanYieldPolicySBE* yieldPolicy,
    boost::optional<SlotId> collatorSlot,
    const PlanNodeId nodeId,
    SlotIdGenerator& slotIdGenerator,
    FrameIdGenerator& frameIdGenerator,
    RuntimeEnvironment* env) {
    CurOp::get(state.opCtx)->debug().indexedLoopJoin += 1;

    const auto foreignCollUUID = foreignColl->uuid();
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
    iamMap.insert({indexName, indexAccessMethod});

    // Build the outer branch that produces the correlated local key slot.
    auto [localKeysSetSlot, localKeysSetStage] = buildKeySet(JoinSide::Local,
                                                             std::move(localStage),
                                                             localRecordSlot,
                                                             localFieldName,
                                                             nodeId,
                                                             slotIdGenerator,
                                                             state.allowDiskUse);

    // Unwind local keys one by one into 'singleLocalValueSlot'.
    auto singleLocalValueSlot = slotIdGenerator.generate();
    auto unwindLocalKeysStage = makeS<UnwindStage>(makeLimitCoScanTree(nodeId, 1),
                                                   localKeysSetSlot /* inSlot */,
                                                   singleLocalValueSlot /* outField */,
                                                   slotIdGenerator.generate() /* outIndex */,
                                                   true /* preserveNullAndEmptyArrays */,
                                                   nodeId);

    // We need to lookup value in 'singleLocalValueSlot' in the index defined on the foreign
    // collection. To do this, we need to generate set of point intervals corresponding to this
    // value. Single value can correspond to multiple point intervals:
    // - Null values:
    //   a. [Null, Null]
    //   b. [Undefined, Undefined]
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
    //       cfilter isNull(rawValue)
    //       project pointValue = Undefined
    //       limit 1
    //       coscan
    //       ,
    //       // Branch 2
    //       filter isArray(rawValue) && !isMember(pointValue, localKeyValueSet)
    //       project pointValue = fillEmpty(
    //           getElement(rawValue, 0),
    //           Undefined
    //       )
    //       limit 1
    //       coscan
    //       ,
    //       // Branch 3
    //       project pointValue = rawValue
    //       limit 1
    //       coscan
    //   ]
    //
    // For null values, only branches (1) and (3) produce values. For array values, only branches
    // (2) and (3) produce values. For all other types, only (3) produces value.
    auto nullBranchOutput = slotIdGenerator.generate();
    auto nullBranch = makeProjectStage(makeLimitCoScanTree(nodeId, 1),
                                       nodeId,
                                       nullBranchOutput,
                                       makeConstant(TypeTags::bsonUndefined, 0));
    nullBranch = makeS<FilterStage<true /*IsConst*/>>(
        std::move(nullBranch), makeFunction("isNull", makeVariable(singleLocalValueSlot)), nodeId);

    auto arrayBranchOutput = slotIdGenerator.generate();
    auto arrayBranch =
        makeProjectStage(makeLimitCoScanTree(nodeId, 1),
                         nodeId,
                         arrayBranchOutput,
                         makeFunction("fillEmpty",
                                      makeFunction("getElement",
                                                   makeVariable(singleLocalValueSlot),
                                                   makeConstant(TypeTags::NumberInt32, 0)),
                                      makeConstant(TypeTags::bsonUndefined, 0)));
    auto shouldProduceSeekForArray =
        makeBinaryOp(EPrimBinary::logicAnd,
                     makeFunction("isArray", makeVariable(singleLocalValueSlot)),
                     makeUnaryOp(EPrimUnary::logicNot,
                                 makeFunction("isMember",
                                              makeVariable(arrayBranchOutput),
                                              makeVariable(localKeysSetSlot))));
    arrayBranch = makeS<FilterStage<false /*IsConst*/>>(
        std::move(arrayBranch), std::move(shouldProduceSeekForArray), nodeId);

    auto valueBranchOutput = slotIdGenerator.generate();
    auto valueBranch = makeProjectStage(makeLimitCoScanTree(nodeId, 1),
                                        nodeId,
                                        valueBranchOutput,
                                        makeVariable(singleLocalValueSlot));

    auto valueForIndexBounds = slotIdGenerator.generate();
    auto valueGeneratorStage = makeS<UnionStage>(
        makeSs(std::move(nullBranch), std::move(arrayBranch), std::move(valueBranch)),
        makeVector(makeSV(nullBranchOutput), makeSV(arrayBranchOutput), makeSV(valueBranchOutput)),
        makeSV(valueForIndexBounds),
        nodeId);

    // For hashed indexes, we need to hash value before computing keystrings.
    if (index.type == INDEX_HASHED) {
        auto rawValueSlot = valueForIndexBounds;
        valueForIndexBounds = slotIdGenerator.generate();
        valueGeneratorStage =
            makeProjectStage(std::move(valueGeneratorStage),
                             nodeId,
                             valueForIndexBounds,
                             makeFunction("shardHash", makeVariable(rawValueSlot)));
    }

    // Calculate the low key and high key of each individual local field. They are stored in
    // 'lowKeySlot' and 'highKeySlot', respectively. These two slots will be made available in
    // the loop join stage to perform index seek. We also set 'indexIdSlot' and
    // 'indexKeyPatternSlot' constants for the seek stage later to perform consistency check.
    auto lowKeySlot = slotIdGenerator.generate();
    auto highKeySlot = slotIdGenerator.generate();
    auto indexIdSlot = slotIdGenerator.generate();
    auto indexKeyPatternSlot = slotIdGenerator.generate();
    auto [_, indexKeyPatternValue] =
        copyValue(TypeTags::bsonObject, bitcastFrom<const char*>(index.keyPattern.objdata()));

    auto makeNewKeyStringCall = [&](KeyString::Discriminator discriminator) {
        StringData functionName = "ks";
        EExpression::Vector args;
        args.emplace_back(
            makeConstant(value::TypeTags::NumberInt64, static_cast<int64_t>(indexVersion)));
        args.emplace_back(makeConstant(value::TypeTags::NumberInt32, indexOrdering.getBits()));
        args.emplace_back(makeVariable(valueForIndexBounds));
        args.emplace_back(
            makeConstant(value::TypeTags::NumberInt64, static_cast<int64_t>(discriminator)));
        if (collatorSlot) {
            functionName = "collKs";
            args.emplace_back(makeVariable(*collatorSlot));
        }
        return makeE<EFunction>(functionName, std::move(args));
    };
    auto indexBoundKeyStage =
        makeProjectStage(std::move(valueGeneratorStage),
                         nodeId,
                         lowKeySlot,
                         makeNewKeyStringCall(KeyString::Discriminator::kExclusiveBefore),
                         highKeySlot,
                         makeNewKeyStringCall(KeyString::Discriminator::kExclusiveAfter),
                         indexIdSlot,
                         makeConstant(indexName),
                         indexKeyPatternSlot,
                         makeConstant(value::TypeTags::bsonObject, indexKeyPatternValue));

    // To ensure that we compute index bounds for all local values, introduce loop join, where
    // unwinding of local values happens on the right side and index generation happens on the left
    // side.
    indexBoundKeyStage = makeS<LoopJoinStage>(std::move(unwindLocalKeysStage),
                                              std::move(indexBoundKeyStage),
                                              makeSV() /* outerProjects */,
                                              makeSV(singleLocalValueSlot) /* outerCorrelated */,
                                              nullptr /* predicate */,
                                              nodeId);

    // Perform the index seek based on the 'lowKeySlot' and 'highKeySlot' from the outer side.
    // The foreign record id of the seek is stored in 'foreignRecordIdSlot'. We also keep
    // 'indexKeySlot' and 'snapshotIdSlot' for the seek stage later to perform consistency
    // check.
    auto foreignRecordIdSlot = slotIdGenerator.generate();
    auto indexKeySlot = slotIdGenerator.generate();
    auto snapshotIdSlot = slotIdGenerator.generate();
    auto ixScanStage = makeS<IndexScanStage>(foreignCollUUID,
                                             indexName,
                                             true /* forward */,
                                             indexKeySlot,
                                             foreignRecordIdSlot,
                                             snapshotIdSlot,
                                             IndexKeysInclusionSet{} /* indexKeysToInclude */,
                                             makeSV() /* vars */,
                                             makeVariable(lowKeySlot),
                                             makeVariable(highKeySlot),
                                             yieldPolicy,
                                             nodeId);

    // Loop join the low key and high key generation with the index seek stage to produce the
    // foreign record id to seek.
    auto ixScanNljStage =
        makeS<LoopJoinStage>(std::move(indexBoundKeyStage),
                             std::move(ixScanStage),
                             makeSV(indexIdSlot, indexKeyPatternSlot) /* outerProjects */,
                             makeSV(lowKeySlot, highKeySlot) /* outerCorrelated */,
                             nullptr /* predicate */,
                             nodeId);

    // It is possible for the same record to be returned multiple times when the index is multikey
    // (contains arrays). Consider an example where local values set is '(1, 2)' and we have a
    // document with foreign field value '[1, 2]'. The same document will be returned twice:
    //  - On the first index seek, where we are looking for value '1'
    //  - On the second index seek, where we are looking for value '2'
    // To avoid such situation, we are placing 'unique' stage to prevent repeating records from
    // appearing in the result.
    if (index.multikey) {
        ixScanNljStage =
            makeS<UniqueStage>(std::move(ixScanNljStage), makeSV(foreignRecordIdSlot), nodeId);
    }

    // Loop join the foreign record id produced by the index seek on the outer side with seek
    // stage on the inner side to get matched foreign documents. The foreign documents are
    // stored in 'foreignRecordSlot'. We also pass in 'snapshotIdSlot', 'indexIdSlot',
    // 'indexKeySlot' and 'indexKeyPatternSlot' to perform index consistency check during the
    // seek.
    auto [foreignRecordSlot, __, scanNljStage] = makeLoopJoinForFetch(std::move(ixScanNljStage),
                                                                      foreignRecordIdSlot,
                                                                      snapshotIdSlot,
                                                                      indexIdSlot,
                                                                      indexKeySlot,
                                                                      indexKeyPatternSlot,
                                                                      foreignColl,
                                                                      iamMap,
                                                                      nodeId,
                                                                      makeSV() /* slotsToForward */,
                                                                      slotIdGenerator);

    // 'buildForeignMatches()' filters the foreign records, returned by the index scan, to match
    // those in 'localKeysSetSlot'. This is necessary because some values are encoded with the same
    // value in BTree index, such as undefined, null and empty array. In hashed indexes, hash
    // collisions are possible.
    auto [foreignGroupSlot, foreignGroupStage] = buildForeignMatches(localKeysSetSlot,
                                                                     std::move(scanNljStage),
                                                                     foreignRecordSlot,
                                                                     foreignFieldName,
                                                                     collatorSlot,
                                                                     nodeId,
                                                                     slotIdGenerator,
                                                                     state.allowDiskUse);

    // 'foreignGroupStage' should not participate in trial run tracking as the number of reads
    // that it performs should not influence planning decisions for 'localKeysSetStage'.
    foreignGroupStage->disableTrialRunTracking();

    // The top level loop join stage that joins each local field with the matched foreign
    // documents.
    auto nljStage = makeS<LoopJoinStage>(std::move(localKeysSetStage),
                                         std::move(foreignGroupStage),
                                         makeSV(localRecordSlot) /* outerProjects */,
                                         makeSV(localKeysSetSlot) /* outerCorrelated */,
                                         nullptr,
                                         nodeId);
    return {foreignGroupSlot, std::move(nljStage)};
}

std::pair<SlotId /*matched docs*/, std::unique_ptr<sbe::PlanStage>> buildHashJoinLookupStage(
    StageBuilderState& state,
    std::unique_ptr<sbe::PlanStage> localStage,
    SlotId localRecordSlot,
    const FieldPath& localFieldName,
    std::unique_ptr<sbe::PlanStage> foreignStage,
    SlotId foreignRecordSlot,
    const FieldPath& foreignFieldName,
    boost::optional<SlotId> collatorSlot,
    const PlanNodeId nodeId,
    SlotIdGenerator& slotIdGenerator) {
    CurOp::get(state.opCtx)->debug().hashLookup += 1;

    // Build the outer branch that produces the set of local key values.
    auto [localKeySlot, outerRootStage] = buildKeySet(JoinSide::Local,
                                                      std::move(localStage),
                                                      localRecordSlot,
                                                      localFieldName,
                                                      nodeId,
                                                      slotIdGenerator,
                                                      state.allowDiskUse);

    // Build the inner branch that produces the set of foreign key values.
    auto [foreignKeySlot, foreignKeyStage] = buildKeySet(JoinSide::Foreign,
                                                         std::move(foreignStage),
                                                         foreignRecordSlot,
                                                         foreignFieldName,
                                                         nodeId,
                                                         slotIdGenerator,
                                                         state.allowDiskUse);

    // 'foreignKeyStage' should not participate in trial run tracking as the number of
    // reads that it performs should not influence planning decisions for 'outerRootStage'.
    foreignKeyStage->disableTrialRunTracking();

    // Build lookup stage that matches the local and foreign rows and aggregates the
    // foreign values in an array.
    auto lookupAggSlot = slotIdGenerator.generate();
    auto aggs = makeEM(lookupAggSlot,
                       stage_builder::makeFunction("addToArray", makeVariable(foreignRecordSlot)));
    std::unique_ptr<sbe::PlanStage> hl = makeS<HashLookupStage>(std::move(outerRootStage),
                                                                std::move(foreignKeyStage),
                                                                localKeySlot,
                                                                foreignKeySlot,
                                                                makeSV(foreignRecordSlot),
                                                                std::move(aggs),
                                                                collatorSlot,
                                                                nodeId);

    // Add a projection that makes so that empty array is returned if no foreign row were matched.
    auto innerResultSlot = slotIdGenerator.generate();
    auto [emptyArrayTag, emptyArrayVal] = makeNewArray();
    std::unique_ptr<EExpression> innerResultProjection = makeFunction(
        "fillEmpty"_sd, makeVariable(lookupAggSlot), makeConstant(emptyArrayTag, emptyArrayVal));

    std::unique_ptr<sbe::PlanStage> resultStage =
        makeProjectStage(std::move(hl), nodeId, innerResultSlot, std::move(innerResultProjection));

    return {innerResultSlot, std::move(resultStage)};
}

std::pair<SlotId /*matched docs*/, std::unique_ptr<sbe::PlanStage>> buildLookupStage(
    StageBuilderState& state,
    EqLookupNode::LookupStrategy lookupStrategy,
    std::unique_ptr<sbe::PlanStage> localStage,
    SlotId localRecordSlot,
    const FieldPath& localFieldName,
    std::unique_ptr<sbe::PlanStage> foreignStage,
    SlotId foreignRecordSlot,
    const FieldPath& foreignFieldName,
    boost::optional<SlotId> collatorSlot,
    const PlanNodeId nodeId,
    SlotIdGenerator& slotIdGenerator) {
    switch (lookupStrategy) {
        case EqLookupNode::LookupStrategy::kNestedLoopJoin:
            return buildNljLookupStage(state,
                                       std::move(localStage),
                                       localRecordSlot,
                                       localFieldName,
                                       std::move(foreignStage),
                                       foreignRecordSlot,
                                       foreignFieldName.fullPath(),
                                       collatorSlot,
                                       nodeId,
                                       slotIdGenerator);
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
                                            slotIdGenerator);
        default:
            MONGO_UNREACHABLE_TASSERT(5842606);
    }
}

/*
 * Builds a project stage that projects an empty array for each local document.
 */
std::pair<SlotId, std::unique_ptr<sbe::PlanStage>> buildNonExistentForeignCollLookupStage(
    std::unique_ptr<sbe::PlanStage> localStage,
    const PlanNodeId nodeId,
    SlotIdGenerator& slotIdGenerator) {
    auto [emptyArrayTag, emptyArrayVal] = makeNewArray();
    SlotId emptyArraySlot = slotIdGenerator.generate();
    return {emptyArraySlot,
            makeProjectStage(std::move(localStage),
                             nodeId,
                             emptyArraySlot,
                             makeConstant(emptyArrayTag, emptyArrayVal))};
}

std::pair<SlotId, std::unique_ptr<sbe::PlanStage>> buildLookupResultObject(
    std::unique_ptr<sbe::PlanStage> stage,
    SlotId localDocumentSlot,
    SlotId resultArraySlot,
    const FieldPath& fieldPath,
    const PlanNodeId nodeId,
    SlotIdGenerator& slotIdGenerator,
    bool shouldProduceBson) {
    const int32_t pathLength = fieldPath.getPathLength();

    // Extract values of all fields along the path except the last one.
    auto fieldSlots = slotIdGenerator.generateMultiple(pathLength - 1);
    for (int32_t i = 0; i < pathLength - 1; i++) {
        const auto fieldName = fieldPath.getFieldName(i);
        const auto inputSlot = i == 0 ? localDocumentSlot : fieldSlots[i - 1];
        stage = makeProjectStage(
            std::move(stage),
            nodeId,
            fieldSlots[i],
            makeFunction("getField"_sd, makeVariable(inputSlot), makeConstant(fieldName)));
    }

    // Construct new objects for each path level.
    auto objectSlots = slotIdGenerator.generateMultiple(pathLength);
    for (int32_t i = pathLength - 1; i >= 0; i--) {
        const auto rootObjectSlot = i == 0 ? localDocumentSlot : fieldSlots[i - 1];
        const auto fieldName = fieldPath.getFieldName(i).toString();
        const auto valueSlot = i == pathLength - 1 ? resultArraySlot : objectSlots[i + 1];
        if (shouldProduceBson) {
            stage =
                makeS<MakeBsonObjStage>(std::move(stage),
                                        objectSlots[i],                        /* objSlot */
                                        rootObjectSlot,                        /* rootSlot */
                                        MakeBsonObjStage::FieldBehavior::drop, /* fieldBehaviour */
                                        std::vector<std::string>{},            /* fields */
                                        std::vector<std::string>{fieldName},   /* projectFields */
                                        SlotVector{valueSlot},                 /* projectVars */
                                        true,                                  /* forceNewObject */
                                        false,                                 /* returnOldObject */
                                        nodeId);
        } else {
            stage = makeS<MakeObjStage>(std::move(stage),
                                        objectSlots[i],                        /* objSlot */
                                        rootObjectSlot,                        /* rootSlot */
                                        MakeBsonObjStage::FieldBehavior::drop, /* fieldBehaviour */
                                        std::vector<std::string>{},            /* fields */
                                        std::vector<std::string>{fieldName},   /* projectFields */
                                        SlotVector{valueSlot},                 /* projectVars */
                                        true,                                  /* forceNewObject */
                                        false,                                 /* returnOldObject */
                                        nodeId);
        }
    }

    return {objectSlots.front(), std::move(stage)};
}
}  // namespace

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildLookup(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    const auto eqLookupNode = static_cast<const EqLookupNode*>(root);
    if (eqLookupNode->lookupStrategy == EqLookupNode::LookupStrategy::kHashJoin) {
        _state.data->foreignHashJoinCollections.emplace(eqLookupNode->foreignCollection);
    }

    // $lookup creates its own output documents.
    _shouldProduceRecordIdSlot = false;

    auto localReqs = reqs.copy().set(kResult);
    auto [localStage, localOutputs] = build(eqLookupNode->children[0].get(), localReqs);
    SlotId localDocumentSlot = localOutputs.get(PlanStageSlots::kResult);

    auto [matchedDocumentsSlot, foreignStage] = [&, localStage = std::move(localStage)]() mutable
        -> std::pair<SlotId, std::unique_ptr<sbe::PlanStage>> {
        const auto& foreignColl =
            _collections.lookupCollection(NamespaceString(eqLookupNode->foreignCollection));

        boost::optional<SlotId> collatorSlot = _state.data->env->getSlotIfExists("collator"_sd);
        switch (eqLookupNode->lookupStrategy) {
            // When foreign collection doesn't exist, we create stages that simply append empty
            // arrays to each local document and do not consider the case that foreign collection
            // may be created during the query, since we cannot easily create dynamic plan stages
            // and it has messier semantics. Builds a project stage that projects an empty array for
            // each local document.
            case EqLookupNode::LookupStrategy::kNonExistentForeignCollection: {
                return buildNonExistentForeignCollLookupStage(
                    std::move(localStage), eqLookupNode->nodeId(), _slotIdGenerator);
            }
            case EqLookupNode::LookupStrategy::kIndexedLoopJoin: {
                tassert(
                    6357201,
                    "$lookup using index join should have one child and a populated index entry",
                    eqLookupNode->children.size() == 1 && eqLookupNode->idxEntry);

                return buildIndexJoinLookupStage(_state,
                                                 std::move(localStage),
                                                 localDocumentSlot,
                                                 eqLookupNode->joinFieldLocal,
                                                 eqLookupNode->joinFieldForeign,
                                                 foreignColl,
                                                 *eqLookupNode->idxEntry,
                                                 _data.iamMap,
                                                 _yieldPolicy,
                                                 collatorSlot,
                                                 eqLookupNode->nodeId(),
                                                 _slotIdGenerator,
                                                 _frameIdGenerator,
                                                 _data.env);
            }
            case EqLookupNode::LookupStrategy::kNestedLoopJoin:
            case EqLookupNode::LookupStrategy::kHashJoin: {
                auto numChildren = eqLookupNode->children.size();
                tassert(6355300, "An EqLookupNode can only have one child", numChildren == 1);

                auto foreignResultSlot = _slotIdGenerator.generate();
                auto foreignRecordIdSlot = _slotIdGenerator.generate();

                auto foreignStage = makeS<ScanStage>(foreignColl->uuid(),
                                                     foreignResultSlot,
                                                     foreignRecordIdSlot,
                                                     boost::none /* snapshotIdSlot */,
                                                     boost::none /* indexIdSlot */,
                                                     boost::none /* indexKeySlot */,
                                                     boost::none /* indexKeyPatternSlot */,
                                                     boost::none /* tsSlot */,
                                                     std::vector<std::string>{} /* fields */,
                                                     makeSV() /* vars */,
                                                     boost::none /* seekKeySlot */,
                                                     true /* forward */,
                                                     _yieldPolicy,
                                                     eqLookupNode->nodeId(),
                                                     ScanCallbacks{});

                return buildLookupStage(_state,
                                        eqLookupNode->lookupStrategy,
                                        std::move(localStage),
                                        localDocumentSlot,
                                        eqLookupNode->joinFieldLocal,
                                        std::move(foreignStage),
                                        foreignResultSlot,
                                        eqLookupNode->joinFieldForeign,
                                        collatorSlot,
                                        eqLookupNode->nodeId(),
                                        _slotIdGenerator);
            }
            default:
                MONGO_UNREACHABLE_TASSERT(5842605);
        }
    }();

    auto [resultSlot, resultStage] = buildLookupResultObject(std::move(foreignStage),
                                                             localDocumentSlot,
                                                             matchedDocumentsSlot,
                                                             eqLookupNode->joinField,
                                                             eqLookupNode->nodeId(),
                                                             _slotIdGenerator,
                                                             eqLookupNode->shouldProduceBson);

    PlanStageSlots outputs;
    outputs.set(kResult, resultSlot);
    return {std::move(resultStage), std::move(outputs)};
}

}  // namespace mongo::stage_builder
