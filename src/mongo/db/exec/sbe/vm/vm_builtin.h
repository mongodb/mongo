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

#pragma once

#include "mongo/db/exec/sbe/vm/vm_types.h"

#include <string>

namespace mongo {
namespace sbe {
namespace vm {
/**
 * Enumeration of SBE VM built-in functions. These are dispatched by ByteCode::dispatchBuiltin() in
 * vm_builtin.cpp. An enum value 'foo' refers to a C++ implementing function named builtinFoo().
 *
 * See also struct Instruction for "functions" like 'setField' that are implemented as single VM
 * instructions.
 *
 * Builtins which can fit into one byte and have small arity are encoded using a special instruction
 * tag, functionSmall.
 */
using SmallBuiltinType = uint8_t;
enum class Builtin : uint16_t {
    split,
    regexMatch,
    replaceOne,  // replace first occurrence of a specified substring with a diffferent substring
    dateDiff,
    dateParts,
    dateToParts,
    isoDateToParts,
    dayOfYear,
    dayOfMonth,
    dayOfWeek,
    datePartsWeekYear,
    dateToString,
    dateFromString,
    dateFromStringNoThrow,
    dropFields,
    newArray,  // create a new array from the top 'arity' values on the stack
    keepFields,
    newArrayFromRange,
    newObj,      // create a new object from 'arity' alternating field names and values on the stack
    newBsonObj,  // same as 'newObj', except it creates a BSON object
    ksToString,  // KeyString to string
    newKs,       // new KeyString
    collNewKs,   // new KeyString (with collation)
    abs,         // absolute value
    ceil,
    floor,
    trunc,
    exp,
    ln,
    log10,
    sqrt,
    pow,
    addToArray,        // agg function to append to an array
    addToArrayCapped,  // agg function to append to an array, fails when the array reaches specified
                       // size
    mergeObjects,      // agg function to merge BSON documents
    addToSet,          // agg function to append to a set
    addToSetCapped,    // agg function to append to a set, fails when the set reaches specified size
    collAddToSet,      // agg function to append to a set (with collation)
    collAddToSetCapped,  // agg function to append to a set (with collation), fails when the set
                         // reaches specified size

    setUnionCapped,  // Agg function to add the elements of an array to a set, fails when the set
                     // reaches specified size.
    collSetUnionCapped,  // Agg function to add the elements of an array to a set (with collation),
                         // fails when the set reaches the specified size.

    // Special double summation.
    doubleDoubleSum,
    // Accumulator to merge simple sums into a double double summation.
    convertSimpleSumToDoubleDoubleSum,
    // A variant of the standard sum aggregate function which maintains a DoubleDouble as the
    // accumulator's underlying state.
    aggDoubleDoubleSum,
    // Converts a DoubleDouble sum into a single numeric scalar for use once the summation is
    // complete.
    doubleDoubleSumFinalize,
    // Converts a partial sum into a format suitable for serialization over the wire to the merging
    // node. The merging node expects the internal state of the DoubleDouble summation to be
    // serialized in a particular format.
    doubleDoublePartialSumFinalize,
    // An agg function which can be used to sum a sequence of DoubleDouble inputs, producing the
    // resulting total as a DoubleDouble.
    aggMergeDoubleDoubleSums,

    // Implements Welford's online algorithm for computing sample or population standard deviation
    // in a single pass.
    aggStdDev,
    // Combines standard deviations that have been partially computed on a subset of the data
    // using Welford's online algorithm.
    aggMergeStdDevs,

    stdDevPopFinalize,
    stdDevSampFinalize,
    bitTestZero,      // test bitwise mask & value is zero
    bitTestMask,      // test bitwise mask & value is mask
    bitTestPosition,  // test BinData with a bit position list
    bsonSize,         // implements $bsonSize
    strLenBytes,      // implements $strLenBytes
    strLenCP,         // implements $strLenCP
    substrBytes,      // implements $substrBytes
    substrCP,         // implements $substrCP
    toUpper,
    toLower,
    coerceToBool,
    coerceToString,
    concat,
    concatArrays,
    trim,
    ltrim,
    rtrim,

    // Agg function to concatenate arrays, failing when the accumulator reaches a specified size.
    aggConcatArraysCapped,

    concatArraysCapped,  // Agg function to add the elements of an array to an accumulator array,
                         // fails when the array reaches specified size.

    // Agg functions to compute the set union of two arrays (no size cap).
    aggSetUnion,
    aggCollSetUnion,
    // Agg functions to compute the set union of two arrays (with a size cap).
    aggSetUnionCapped,
    aggCollSetUnionCapped,

    acos,
    acosh,
    asin,
    asinh,
    atan,
    atanh,
    atan2,
    cos,
    cosh,
    degreesToRadians,
    radiansToDegrees,
    sin,
    sinh,
    tan,
    tanh,
    rand,  // implements $rand
    round,
    isMember,
    collIsMember,
    indexOfBytes,
    indexOfCP,
    isDayOfWeek,
    isTimeUnit,
    isTimezone,
    isValidToStringFormat,
    validateFromStringFormat,
    setUnion,
    setIntersection,
    setDifference,
    setEquals,
    setIsSubset,
    collSetUnion,
    collSetIntersection,
    collSetDifference,
    collSetEquals,
    collSetIsSubset,
    runJsPredicate,
    regexCompile,  // compile <pattern, options> into value::pcreRegex
    regexFind,
    regexFindAll,
    shardFilter,
    shardHash,
    extractSubArray,
    isArrayEmpty,
    reverseArray,
    sortArray,
    dateAdd,
    hasNullBytes,
    getRegexPattern,
    getRegexFlags,
    hash,
    ftsMatch,
    generateSortKey,
    generateCheapSortKey,
    sortKeyComponentVectorGetElement,
    sortKeyComponentVectorToArray,

    makeObj,
    makeBsonObj,
    tsSecond,
    tsIncrement,
    typeMatch,
    dateTrunc,
    getSortKeyAsc,          // helper functions for computation of sort keys
    getSortKeyDesc,         // helper functions for computation of sort keys
    getNonLeafSortKeyAsc,   // helper functions for computation of sort keys
    getNonLeafSortKeyDesc,  // helper functions for computation of sort keys
    year,
    month,
    hour,
    minute,
    second,
    millisecond,
    week,
    isoWeekYear,
    isoDayOfWeek,
    isoWeek,
    objectToArray,
    setToArray,
    arrayToObject,
    avgOfArray,  // Returns the $avg of an array.
    maxOfArray,  // Returns the $max element of an array.
    minOfArray,  // Returns the $min element of an array.
    stdDevPop,   // Returns the $stdDevPop of an array.
    stdDevSamp,  // Returns the $stdDevSamp of an array.
    sumOfArray,  // Returns the $sum of an array
    unwindArray,
    arrayToSet,
    collArrayToSet,

    fillType,

    aggFirstNNeedsMoreInput,
    aggFirstN,
    aggFirstNMerge,
    aggFirstNFinalize,
    aggLastN,
    aggLastNMerge,
    aggLastNFinalize,
    aggTopN,
    aggTopNArray,
    aggTopNMerge,
    aggTopNFinalize,
    aggBottomN,
    aggBottomNArray,
    aggBottomNMerge,
    aggBottomNFinalize,
    aggMaxN,
    aggMaxNMerge,
    aggMaxNFinalize,
    aggMinN,
    aggMinNMerge,
    aggMinNFinalize,
    aggRank,
    aggRankColl,
    aggDenseRank,
    aggDenseRankColl,
    aggRankFinalize,
    aggExpMovingAvg,
    aggExpMovingAvgFinalize,
    aggRemovableSumAdd,
    aggRemovableSumRemove,
    aggRemovableSumFinalize,
    aggIntegralInit,
    aggIntegralAdd,
    aggIntegralRemove,
    aggIntegralFinalize,
    aggDerivativeFinalize,
    aggCovarianceAdd,
    aggCovarianceRemove,
    aggCovarianceSampFinalize,
    aggCovariancePopFinalize,
    aggRemovablePushAdd,
    aggRemovablePushRemove,
    aggRemovablePushFinalize,
    aggRemovableConcatArraysInit,
    aggRemovableConcatArraysAdd,
    aggRemovableConcatArraysRemove,
    aggRemovableConcatArraysFinalize,
    aggRemovableStdDevAdd,
    aggRemovableStdDevRemove,
    aggRemovableStdDevSampFinalize,
    aggRemovableStdDevPopFinalize,
    aggRemovableAvgFinalize,
    aggLinearFillCanAdd,
    aggLinearFillAdd,
    aggLinearFillFinalize,
    aggRemovableFirstNInit,
    aggRemovableFirstNAdd,
    aggRemovableFirstNRemove,
    aggRemovableFirstNFinalize,
    aggRemovableLastNInit,
    aggRemovableLastNAdd,
    aggRemovableLastNRemove,
    aggRemovableLastNFinalize,

    // $addToSet and $setUnion share some common functionality.
    aggRemovableSetCommonInit,
    aggRemovableSetCommonCollInit,
    aggRemovableAddToSetAdd,
    aggRemovableAddToSetRemove,
    aggRemovableSetUnionAdd,
    aggRemovableSetUnionRemove,
    aggRemovableSetCommonFinalize,

    aggRemovableMinMaxNCollInit,
    aggRemovableMinMaxNInit,
    aggRemovableMinMaxNAdd,
    aggRemovableMinMaxNRemove,
    aggRemovableMinNFinalize,
    aggRemovableMaxNFinalize,
    aggRemovableTopNInit,
    aggRemovableTopNAdd,
    aggRemovableTopNRemove,
    aggRemovableTopNFinalize,
    aggRemovableBottomNInit,
    aggRemovableBottomNAdd,
    aggRemovableBottomNRemove,
    aggRemovableBottomNFinalize,

    // Additional one-byte builtins go here.

    // Start of 2 byte builtins.
    valueBlockExists = 256,
    valueBlockTypeMatch,
    valueBlockIsTimezone,
    valueBlockFillEmpty,
    valueBlockFillEmptyBlock,
    valueBlockFillType,
    valueBlockAggMin,
    valueBlockAggMax,
    valueBlockAggCount,
    valueBlockAggSum,
    valueBlockAggDoubleDoubleSum,
    valueBlockAggTopN,
    valueBlockAggTopNArray,
    valueBlockAggBottomN,
    valueBlockAggBottomNArray,
    valueBlockDateDiff,
    valueBlockDateTrunc,
    valueBlockDateAdd,
    valueBlockTrunc,
    valueBlockRound,
    valueBlockAdd,
    valueBlockSub,
    valueBlockMult,
    valueBlockDiv,
    valueBlockGtScalar,
    valueBlockGteScalar,
    valueBlockEqScalar,
    valueBlockNeqScalar,
    valueBlockLtScalar,
    valueBlockLteScalar,
    valueBlockCmp3wScalar,
    valueBlockCombine,
    valueBlockLogicalAnd,
    valueBlockLogicalOr,
    valueBlockLogicalNot,
    valueBlockNewFill,
    valueBlockSize,
    valueBlockNone,
    valueBlockIsMember,
    valueBlockCoerceToBool,
    valueBlockMod,
    valueBlockConvert,
    valueBlockGetSortKeyAsc,
    valueBlockGetSortKeyDesc,

    cellFoldValues_F,
    cellFoldValues_P,
    cellBlockGetFlatValuesBlock,

    currentDate,
};  // enum class Builtin

std::string builtinToString(Builtin b);

}  // namespace vm
}  // namespace sbe
}  // namespace mongo
