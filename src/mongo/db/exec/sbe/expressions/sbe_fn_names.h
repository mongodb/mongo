/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"

/**
 * Enum and string constants for all function names that can appear in ABT FunctionCall nodes.
 *
 * This includes:
 *  - SBE built-in functions dispatched via kBuiltinFunctions/kInstrFunctions in expression.cpp
 *  - ABT-level constructs that are resolved before or during lowering (e.g. kGetParam, which
 *    is lowered to EVariable, and kBlockTraverseFPlaceholder, which is resolved by the vectorizer)
 *
 * EFn is an enum class whose integer values index into a parallel array of string names in
 * sbe_fn_names.cpp. Use toString(EFn) to get the string and fromString(StringData) to reverse
 * the mapping.
 *
 * The authoritative source for VM dispatch is the kBuiltinFunctions and kInstrFunctions maps in
 * src/mongo/db/exec/sbe/expressions/expression.cpp. Every key in those maps must have a
 * corresponding EFn value here.
 *
 * ### Adding a new built-in function
 *
 * 1. sbe_fn_names.h (this file) — add a new enumerator to EFn in alphabetical order by the
 *    string name of the function (not by the C++ identifier). The enum values are used as
 *    array indices into kEFnByName[], so the order must remain alphabetical by string value.
 *    Increment kNumFunctions by adjusting the sentinel; it is the last enumerator and its
 *    value is computed automatically.
 *
 * 2. sbe_fn_names.cpp — add a {StringData, EFn} entry to kEFnByName[] in the same
 *    alphabetical position as step 1. This array must remain sorted by string value
 *    and its enum values must stay in sequential order (both are compile-time checked).
 *    If the function is only reachable from the ABT/optimizer layer under a different
 *    dollar-prefixed name (e.g. "$push" -> kAddToArray), also add an entry to kEFnAliases[],
 *    keeping that array sorted as well.
 *
 * 3. expression.cpp — add the function to kBuiltinFunctions (or kInstrFunctions if it is an
 *    "instr" function). Provide the BuiltinFn descriptor: arity and a pointer to the vm::Vm
 *    member that implements it.
 *    Skip this step for ABT-only constructs that are never lowered to an EFunction call (e.g.
 *    kBlockTraverseFPlaceholder, resolved by the vectorizer; kGetParam, lowered to EVariable).
 *
 * 4. vm.h / vm.cpp — declare and implement the actual vm::Vm::builtin<FunctionName> method.
 *    Skip this step for ABT-only constructs (same caveat as step 3).
 */
namespace mongo::sbe {

// clang-format off

/**
 * Enum of all SBE built-in function names.
 *
 * The integer values are indices into the kEFnByName[] array in sbe_fn_names.cpp,
 * which is sorted alphabetically by string value. This allows both O(1) toString()
 * (direct array index) and O(log n) fromString() (binary search) using the same table.
 *
 * kNumFunctions is a sentinel whose integer value equals the count of valid enum
 * values and also the length of kEFnByName[].
 */
enum class EFn : uint16_t {
    kAbs = 0,
    kAcos,
    kAcosh,
    kAddToArray,
    kAddToArrayCapped,
    kAddToSet,
    kAddToSetCapped,
    kAggBottomN,
    kAggBottomNArray,
    kAggBottomNFinalize,
    kAggBottomNMerge,
    kAggCollSetUnion,
    kAggCollSetUnionCapped,
    kAggConcatArraysCapped,
    kAggCovarianceAdd,
    kAggCovariancePopFinalize,
    kAggCovarianceRemove,
    kAggCovarianceSampFinalize,
    kAggDenseRank,
    kAggDenseRankColl,
    kAggDerivativeFinalize,
    kAggDoubleDoubleSum,
    kAggExpMovingAvg,
    kAggExpMovingAvgFinalize,
    kAggFirstN,
    kAggFirstNFinalize,
    kAggFirstNMerge,
    kAggFirstNNeedsMoreInput,
    kAggIntegralAdd,
    kAggIntegralFinalize,
    kAggIntegralInit,
    kAggIntegralRemove,
    kAggLastN,
    kAggLastNFinalize,
    kAggLastNMerge,
    kAggLinearFillAdd,
    kAggLinearFillCanAdd,
    kAggLinearFillFinalize,
    kAggMaxN,
    kAggMaxNFinalize,
    kAggMaxNMerge,
    kAggMergeDoubleDoubleSums,
    kAggMergeStdDevs,
    kAggMinN,
    kAggMinNFinalize,
    kAggMinNMerge,
    kAggRank,
    kAggRankColl,
    kAggRankFinalize,
    kAggRemovableAddToSetAdd,
    kAggRemovableAddToSetRemove,
    kAggRemovableAvgFinalize,
    kAggRemovableBottomNAdd,
    kAggRemovableBottomNFinalize,
    kAggRemovableBottomNInit,
    kAggRemovableBottomNRemove,
    kAggRemovableConcatArraysAdd,
    kAggRemovableConcatArraysFinalize,
    kAggRemovableConcatArraysInit,
    kAggRemovableConcatArraysRemove,
    kAggRemovableFirstNAdd,
    kAggRemovableFirstNFinalize,
    kAggRemovableFirstNInit,
    kAggRemovableFirstNRemove,
    kAggRemovableLastNAdd,
    kAggRemovableLastNFinalize,
    kAggRemovableLastNInit,
    kAggRemovableLastNRemove,
    kAggRemovableMaxNFinalize,
    kAggRemovableMinMaxNAdd,
    kAggRemovableMinMaxNCollInit,
    kAggRemovableMinMaxNInit,
    kAggRemovableMinMaxNRemove,
    kAggRemovableMinNFinalize,
    kAggRemovablePushAdd,
    kAggRemovablePushFinalize,
    kAggRemovablePushRemove,
    kAggRemovableSetCommonCollInit,
    kAggRemovableSetCommonFinalize,
    kAggRemovableSetCommonInit,
    kAggRemovableSetUnionAdd,
    kAggRemovableSetUnionRemove,
    kAggRemovableStdDevAdd,
    kAggRemovableStdDevPopFinalize,
    kAggRemovableStdDevRemove,
    kAggRemovableStdDevSampFinalize,
    kAggRemovableSumAdd,
    kAggRemovableSumFinalize,
    kAggRemovableSumRemove,
    kAggRemovableTopNAdd,
    kAggRemovableTopNFinalize,
    kAggRemovableTopNInit,
    kAggRemovableTopNRemove,
    kAggSetUnion,
    kAggSetUnionCapped,
    kAggState,
    kAggStdDev,
    kAggTopN,
    kAggTopNArray,
    kAggTopNFinalize,
    kAggTopNMerge,
    kArray,
    kArrayToObject,
    kArrayToSet,
    kAsin,
    kAsinh,
    kAtan,
    kAtan2,
    kAtanh,
    kAvgOfArray,
    kBitTestMask,
    kBitTestPosition,
    kBitTestZero,
    kBlockTraverseFPlaceholder,
    kBottom,
    kBottomN,
    kBsonSize,
    kCeil,
    kCellBlockGetFlatValuesBlock,
    kCellFoldValues_F,
    kCellFoldValues_P,
    kCoerceToBool,
    kCoerceToString,
    kCollAddToSet,
    kCollAddToSetCapped,
    kCollArrayToSet,
    kCollComparisonKey,
    kCollIsMember,
    kCollKs,
    kCollMax,
    kCollMin,
    kCollSetDifference,
    kCollSetEquals,
    kCollSetIntersection,
    kCollSetIsSubset,
    kCollSetUnion,
    kCollSetUnionCapped,
    kConcat,
    kConcatArrays,
    kConcatArraysCapped,
    kConvert,
    kConvertSimpleSumToDoubleDoubleSum,
    kCos,
    kCosh,
    kCount,
    kCurrentDate,
    kDateAdd,
    kDateDiff,
    kDateFromString,
    kDateFromStringNoThrow,
    kDateParts,
    kDatePartsWeekYear,
    kDateToParts,
    kDateToString,
    kDateTrunc,
    kDayOfMonth,
    kDayOfWeek,
    kDayOfYear,
    kDegreesToRadians,
    kDoubleDoublePartialSumFinalize,
    kDoubleDoubleSum,
    kDoubleDoubleSumFinalize,
    kDropFields,
    kExists,
    kExp,
    kExtractSubArray,
    kFail,
    kFillType,
    kFirst,
    kFloor,
    kFtsMatch,
    kGenerateCheapSortKey,
    kGenerateSortKey,
    kGetArraySize,
    kGetElement,
    kGetField,
    kGetFieldOrElement,
    kGetNonLeafSortKeyAsc,
    kGetNonLeafSortKeyDesc,
    kGetParam,
    kGetRegexFlags,
    kGetRegexPattern,
    kGetSortKeyAsc,
    kGetSortKeyDesc,
    kHasNullBytes,
    kHash,
    kHour,
    kIndexOfBytes,
    kIndexOfCP,
    kIsArray,
    kIsArrayEmpty,
    kIsBinData,
    kIsDate,
    kIsDayOfWeek,
    kIsInList,
    kIsInfinity,
    kIsKeyString,
    kIsMaxKey,
    kIsMember,
    kIsMinKey,
    kIsNaN,
    kIsNull,
    kIsNumber,
    kIsObject,
    kIsRecordId,
    kIsString,
    kIsTimeUnit,
    kIsTimestamp,
    kIsTimezone,
    kIsValidToStringFormat,
    kIsoDateToParts,
    kIsoDayOfWeek,
    kIsoWeek,
    kIsoWeekYear,
    kKeepFields,
    kKs,
    kLast,
    kLn,
    kLog10,
    kLtrim,
    kMagicTraverseF,
    kMakeBsonObj,
    kMakeObj,
    kMakeOwn,
    kMax,
    kMaxOfArray,
    kMergeObjects,
    kMillisecond,
    kMin,
    kMinOfArray,
    kMinute,
    kMod,
    kMonth,
    kNewArray,
    kNewArrayFromRange,
    kNewBsonObj,
    kNewObj,
    kObjectToArray,
    kPow,
    kRadiansToDegrees,
    kRand,
    kRegexCompile,
    kRegexFind,
    kRegexFindAll,
    kRegexMatch,
    kReplaceOne,
    kReverseArray,
    kRound,
    kRtrim,
    kRunJsPredicate,
    kSecond,
    kSetDifference,
    kSetEquals,
    kSetField,
    kSetIntersection,
    kSetIsSubset,
    kSetToArray,
    kSetUnion,
    kSetUnionCapped,
    kShardFilter,
    kShardHash,
    kSin,
    kSinh,
    kSortArray,
    kSortKeyComponentVectorGetElement,
    kSortKeyComponentVectorToArray,
    kSplit,
    kSqrt,
    kStdDevPop,
    kStdDevPopFinalize,
    kStdDevSamp,
    kStdDevSampFinalize,
    kStrLenBytes,
    kStrLenCP,
    kSubstrBytes,
    kSubstrCP,
    kSum,
    kSumOfArray,
    kTan,
    kTanh,
    kToLower,
    kToUpper,
    kTop,
    kTopN,
    kTraverseF,
    kTraverseP,
    kTrim,
    kTrunc,
    kTsIncrement,
    kTsSecond,
    kTypeMatch,
    kUnwindArray,
    kValidateFromStringFormat,
    kValueBlockAdd,
    kValueBlockAggBottomN,
    kValueBlockAggBottomNArray,
    kValueBlockAggCount,
    kValueBlockAggDoubleDoubleSum,
    kValueBlockAggMax,
    kValueBlockAggMin,
    kValueBlockAggSum,
    kValueBlockAggTopN,
    kValueBlockAggTopNArray,
    kValueBlockApplyLambda,
    kValueBlockCmp3wScalar,
    kValueBlockCoerceToBool,
    kValueBlockCombine,
    kValueBlockConvert,
    kValueBlockDateAdd,
    kValueBlockDateDiff,
    kValueBlockDateTrunc,
    kValueBlockDiv,
    kValueBlockEqScalar,
    kValueBlockExists,
    kValueBlockFillEmpty,
    kValueBlockFillEmptyBlock,
    kValueBlockFillType,
    kValueBlockGetSortKeyAsc,
    kValueBlockGetSortKeyDesc,
    kValueBlockGtScalar,
    kValueBlockGteScalar,
    kValueBlockIsMember,
    kValueBlockIsTimezone,
    kValueBlockLogicalAnd,
    kValueBlockLogicalNot,
    kValueBlockLogicalOr,
    kValueBlockLtScalar,
    kValueBlockLteScalar,
    kValueBlockMod,
    kValueBlockMult,
    kValueBlockNeqScalar,
    kValueBlockNewFill,
    kValueBlockNone,
    kValueBlockRound,
    kValueBlockSize,
    kValueBlockSub,
    kValueBlockTrunc,
    kValueBlockTypeMatch,
    kWeek,
    kYear,
    kZipArrays,

    kNumFunctions,  // sentinel — must be last
};

/**
 * Convert an EFn value to its canonical string representation.
 * Calling with an out-of-range value is a programming error (invariant).
 */
StringData toString(EFn fn);

/**
 * Reverse mapping: look up an EFn by its string name (or a recognised alias
 * such as "$sum").  Triggers a tassert on unknown names: reaching this function
 * with an unrecognized name means the optimizer or plan cache emitted an
 * invalid function name, which is a programmer error, not a recoverable
 * condition.
 */
EFn fromString(StringData name);

// clang-format on

}  // namespace mongo::sbe
