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

#include "mongo/db/exec/sbe/expressions/sbe_fn_names.h"

#include "mongo/base/string_data.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <array>
#include <utility>

namespace mongo::sbe {

// ---------------------------------------------------------------------------
// fromString
//
// The primary lookup table maps canonical string names to EFn values.
// It must be kept sorted by string so std::lower_bound works correctly.
//
// Additionally, a small alias table handles dollar-prefixed MQL accumulator
// names ($sum, $first, etc.) for backward compatibility with serialized forms
// such as plan cache entries.
// ---------------------------------------------------------------------------

namespace {

struct EFnEntry {
    StringData name;
    EFn value;
};

// Sorted by name (same alphabetical order as the enum values).
// Also used by toString(): kEFnByName[static_cast<size_t>(fn)].name gives the string.
// clang-format off
static constexpr EFnEntry kEFnByName[] = {
    {"abs"_sd,                                EFn::kAbs},
    {"acos"_sd,                               EFn::kAcos},
    {"acosh"_sd,                              EFn::kAcosh},
    {"addToArray"_sd,                         EFn::kAddToArray},
    {"addToArrayCapped"_sd,                   EFn::kAddToArrayCapped},
    {"addToSet"_sd,                           EFn::kAddToSet},
    {"addToSetCapped"_sd,                     EFn::kAddToSetCapped},
    {"aggBottomN"_sd,                         EFn::kAggBottomN},
    {"aggBottomNArray"_sd,                    EFn::kAggBottomNArray},
    {"aggBottomNFinalize"_sd,                 EFn::kAggBottomNFinalize},
    {"aggBottomNMerge"_sd,                    EFn::kAggBottomNMerge},
    {"aggCollSetUnion"_sd,                    EFn::kAggCollSetUnion},
    {"aggCollSetUnionCapped"_sd,              EFn::kAggCollSetUnionCapped},
    {"aggConcatArraysCapped"_sd,              EFn::kAggConcatArraysCapped},
    {"aggCovarianceAdd"_sd,                   EFn::kAggCovarianceAdd},
    {"aggCovariancePopFinalize"_sd,           EFn::kAggCovariancePopFinalize},
    {"aggCovarianceRemove"_sd,                EFn::kAggCovarianceRemove},
    {"aggCovarianceSampFinalize"_sd,          EFn::kAggCovarianceSampFinalize},
    {"aggDenseRank"_sd,                       EFn::kAggDenseRank},
    {"aggDenseRankColl"_sd,                   EFn::kAggDenseRankColl},
    {"aggDerivativeFinalize"_sd,              EFn::kAggDerivativeFinalize},
    {"aggDoubleDoubleSum"_sd,                 EFn::kAggDoubleDoubleSum},
    {"aggExpMovingAvg"_sd,                    EFn::kAggExpMovingAvg},
    {"aggExpMovingAvgFinalize"_sd,            EFn::kAggExpMovingAvgFinalize},
    {"aggFirstN"_sd,                          EFn::kAggFirstN},
    {"aggFirstNFinalize"_sd,                  EFn::kAggFirstNFinalize},
    {"aggFirstNMerge"_sd,                     EFn::kAggFirstNMerge},
    {"aggFirstNNeedsMoreInput"_sd,            EFn::kAggFirstNNeedsMoreInput},
    {"aggIntegralAdd"_sd,                     EFn::kAggIntegralAdd},
    {"aggIntegralFinalize"_sd,                EFn::kAggIntegralFinalize},
    {"aggIntegralInit"_sd,                    EFn::kAggIntegralInit},
    {"aggIntegralRemove"_sd,                  EFn::kAggIntegralRemove},
    {"aggLastN"_sd,                           EFn::kAggLastN},
    {"aggLastNFinalize"_sd,                   EFn::kAggLastNFinalize},
    {"aggLastNMerge"_sd,                      EFn::kAggLastNMerge},
    {"aggLinearFillAdd"_sd,                   EFn::kAggLinearFillAdd},
    {"aggLinearFillCanAdd"_sd,                EFn::kAggLinearFillCanAdd},
    {"aggLinearFillFinalize"_sd,              EFn::kAggLinearFillFinalize},
    {"aggMaxN"_sd,                            EFn::kAggMaxN},
    {"aggMaxNFinalize"_sd,                    EFn::kAggMaxNFinalize},
    {"aggMaxNMerge"_sd,                       EFn::kAggMaxNMerge},
    {"aggMergeDoubleDoubleSums"_sd,           EFn::kAggMergeDoubleDoubleSums},
    {"aggMergeStdDevs"_sd,                    EFn::kAggMergeStdDevs},
    {"aggMinN"_sd,                            EFn::kAggMinN},
    {"aggMinNFinalize"_sd,                    EFn::kAggMinNFinalize},
    {"aggMinNMerge"_sd,                       EFn::kAggMinNMerge},
    {"aggRank"_sd,                            EFn::kAggRank},
    {"aggRankColl"_sd,                        EFn::kAggRankColl},
    {"aggRankFinalize"_sd,                    EFn::kAggRankFinalize},
    {"aggRemovableAddToSetAdd"_sd,            EFn::kAggRemovableAddToSetAdd},
    {"aggRemovableAddToSetRemove"_sd,         EFn::kAggRemovableAddToSetRemove},
    {"aggRemovableAvgFinalize"_sd,            EFn::kAggRemovableAvgFinalize},
    {"aggRemovableBottomNAdd"_sd,             EFn::kAggRemovableBottomNAdd},
    {"aggRemovableBottomNFinalize"_sd,        EFn::kAggRemovableBottomNFinalize},
    {"aggRemovableBottomNInit"_sd,            EFn::kAggRemovableBottomNInit},
    {"aggRemovableBottomNRemove"_sd,          EFn::kAggRemovableBottomNRemove},
    {"aggRemovableConcatArraysAdd"_sd,        EFn::kAggRemovableConcatArraysAdd},
    {"aggRemovableConcatArraysFinalize"_sd,   EFn::kAggRemovableConcatArraysFinalize},
    {"aggRemovableConcatArraysInit"_sd,       EFn::kAggRemovableConcatArraysInit},
    {"aggRemovableConcatArraysRemove"_sd,     EFn::kAggRemovableConcatArraysRemove},
    {"aggRemovableFirstNAdd"_sd,              EFn::kAggRemovableFirstNAdd},
    {"aggRemovableFirstNFinalize"_sd,         EFn::kAggRemovableFirstNFinalize},
    {"aggRemovableFirstNInit"_sd,             EFn::kAggRemovableFirstNInit},
    {"aggRemovableFirstNRemove"_sd,           EFn::kAggRemovableFirstNRemove},
    {"aggRemovableLastNAdd"_sd,               EFn::kAggRemovableLastNAdd},
    {"aggRemovableLastNFinalize"_sd,          EFn::kAggRemovableLastNFinalize},
    {"aggRemovableLastNInit"_sd,              EFn::kAggRemovableLastNInit},
    {"aggRemovableLastNRemove"_sd,            EFn::kAggRemovableLastNRemove},
    {"aggRemovableMaxNFinalize"_sd,           EFn::kAggRemovableMaxNFinalize},
    {"aggRemovableMinMaxNAdd"_sd,             EFn::kAggRemovableMinMaxNAdd},
    {"aggRemovableMinMaxNCollInit"_sd,        EFn::kAggRemovableMinMaxNCollInit},
    {"aggRemovableMinMaxNInit"_sd,            EFn::kAggRemovableMinMaxNInit},
    {"aggRemovableMinMaxNRemove"_sd,          EFn::kAggRemovableMinMaxNRemove},
    {"aggRemovableMinNFinalize"_sd,           EFn::kAggRemovableMinNFinalize},
    {"aggRemovablePushAdd"_sd,                EFn::kAggRemovablePushAdd},
    {"aggRemovablePushFinalize"_sd,           EFn::kAggRemovablePushFinalize},
    {"aggRemovablePushRemove"_sd,             EFn::kAggRemovablePushRemove},
    {"aggRemovableSetCommonCollInit"_sd,      EFn::kAggRemovableSetCommonCollInit},
    {"aggRemovableSetCommonFinalize"_sd,      EFn::kAggRemovableSetCommonFinalize},
    {"aggRemovableSetCommonInit"_sd,          EFn::kAggRemovableSetCommonInit},
    {"aggRemovableSetUnionAdd"_sd,            EFn::kAggRemovableSetUnionAdd},
    {"aggRemovableSetUnionRemove"_sd,         EFn::kAggRemovableSetUnionRemove},
    {"aggRemovableStdDevAdd"_sd,              EFn::kAggRemovableStdDevAdd},
    {"aggRemovableStdDevPopFinalize"_sd,      EFn::kAggRemovableStdDevPopFinalize},
    {"aggRemovableStdDevRemove"_sd,           EFn::kAggRemovableStdDevRemove},
    {"aggRemovableStdDevSampFinalize"_sd,     EFn::kAggRemovableStdDevSampFinalize},
    {"aggRemovableSumAdd"_sd,                 EFn::kAggRemovableSumAdd},
    {"aggRemovableSumFinalize"_sd,            EFn::kAggRemovableSumFinalize},
    {"aggRemovableSumRemove"_sd,              EFn::kAggRemovableSumRemove},
    {"aggRemovableTopNAdd"_sd,                EFn::kAggRemovableTopNAdd},
    {"aggRemovableTopNFinalize"_sd,           EFn::kAggRemovableTopNFinalize},
    {"aggRemovableTopNInit"_sd,               EFn::kAggRemovableTopNInit},
    {"aggRemovableTopNRemove"_sd,             EFn::kAggRemovableTopNRemove},
    {"aggSetUnion"_sd,                        EFn::kAggSetUnion},
    {"aggSetUnionCapped"_sd,                  EFn::kAggSetUnionCapped},
    {"aggState"_sd,                           EFn::kAggState},
    {"aggStdDev"_sd,                          EFn::kAggStdDev},
    {"aggTopN"_sd,                            EFn::kAggTopN},
    {"aggTopNArray"_sd,                       EFn::kAggTopNArray},
    {"aggTopNFinalize"_sd,                    EFn::kAggTopNFinalize},
    {"aggTopNMerge"_sd,                       EFn::kAggTopNMerge},
    {"array"_sd,                              EFn::kArray},
    {"arrayToObject"_sd,                      EFn::kArrayToObject},
    {"arrayToSet"_sd,                         EFn::kArrayToSet},
    {"asin"_sd,                               EFn::kAsin},
    {"asinh"_sd,                              EFn::kAsinh},
    {"atan"_sd,                               EFn::kAtan},
    {"atan2"_sd,                              EFn::kAtan2},
    {"atanh"_sd,                              EFn::kAtanh},
    {"avgOfArray"_sd,                         EFn::kAvgOfArray},
    {"bitTestMask"_sd,                        EFn::kBitTestMask},
    {"bitTestPosition"_sd,                    EFn::kBitTestPosition},
    {"bitTestZero"_sd,                        EFn::kBitTestZero},
    {"blockTraverseFPlaceholder"_sd,          EFn::kBlockTraverseFPlaceholder},
    {"bottom"_sd,                             EFn::kBottom},
    {"bottomN"_sd,                            EFn::kBottomN},
    {"bsonSize"_sd,                           EFn::kBsonSize},
    {"ceil"_sd,                               EFn::kCeil},
    {"cellBlockGetFlatValuesBlock"_sd,        EFn::kCellBlockGetFlatValuesBlock},
    {"cellFoldValues_F"_sd,                   EFn::kCellFoldValues_F},
    {"cellFoldValues_P"_sd,                   EFn::kCellFoldValues_P},
    {"coerceToBool"_sd,                       EFn::kCoerceToBool},
    {"coerceToString"_sd,                     EFn::kCoerceToString},
    {"collAddToSet"_sd,                       EFn::kCollAddToSet},
    {"collAddToSetCapped"_sd,                 EFn::kCollAddToSetCapped},
    {"collArrayToSet"_sd,                     EFn::kCollArrayToSet},
    {"collComparisonKey"_sd,                  EFn::kCollComparisonKey},
    {"collIsMember"_sd,                       EFn::kCollIsMember},
    {"collKs"_sd,                             EFn::kCollKs},
    {"collMax"_sd,                            EFn::kCollMax},
    {"collMin"_sd,                            EFn::kCollMin},
    {"collSetDifference"_sd,                  EFn::kCollSetDifference},
    {"collSetEquals"_sd,                      EFn::kCollSetEquals},
    {"collSetIntersection"_sd,                EFn::kCollSetIntersection},
    {"collSetIsSubset"_sd,                    EFn::kCollSetIsSubset},
    {"collSetUnion"_sd,                       EFn::kCollSetUnion},
    {"collSetUnionCapped"_sd,                 EFn::kCollSetUnionCapped},
    {"concat"_sd,                             EFn::kConcat},
    {"concatArrays"_sd,                       EFn::kConcatArrays},
    {"concatArraysCapped"_sd,                 EFn::kConcatArraysCapped},
    {"convert"_sd,                            EFn::kConvert},
    {"convertSimpleSumToDoubleDoubleSum"_sd,  EFn::kConvertSimpleSumToDoubleDoubleSum},
    {"cos"_sd,                                EFn::kCos},
    {"cosh"_sd,                               EFn::kCosh},
    {"count"_sd,                              EFn::kCount},
    {"currentDate"_sd,                        EFn::kCurrentDate},
    {"dateAdd"_sd,                            EFn::kDateAdd},
    {"dateDiff"_sd,                           EFn::kDateDiff},
    {"dateFromString"_sd,                     EFn::kDateFromString},
    {"dateFromStringNoThrow"_sd,              EFn::kDateFromStringNoThrow},
    {"dateParts"_sd,                          EFn::kDateParts},
    {"datePartsWeekYear"_sd,                  EFn::kDatePartsWeekYear},
    {"dateToParts"_sd,                        EFn::kDateToParts},
    {"dateToString"_sd,                       EFn::kDateToString},
    {"dateTrunc"_sd,                          EFn::kDateTrunc},
    {"dayOfMonth"_sd,                         EFn::kDayOfMonth},
    {"dayOfWeek"_sd,                          EFn::kDayOfWeek},
    {"dayOfYear"_sd,                          EFn::kDayOfYear},
    {"degreesToRadians"_sd,                   EFn::kDegreesToRadians},
    {"doubleDoublePartialSumFinalize"_sd,     EFn::kDoubleDoublePartialSumFinalize},
    {"doubleDoubleSum"_sd,                    EFn::kDoubleDoubleSum},
    {"doubleDoubleSumFinalize"_sd,            EFn::kDoubleDoubleSumFinalize},
    {"dropFields"_sd,                         EFn::kDropFields},
    {"exists"_sd,                             EFn::kExists},
    {"exp"_sd,                                EFn::kExp},
    {"extractSubArray"_sd,                    EFn::kExtractSubArray},
    {"fail"_sd,                               EFn::kFail},
    {"fillType"_sd,                           EFn::kFillType},
    {"first"_sd,                              EFn::kFirst},
    {"floor"_sd,                              EFn::kFloor},
    {"ftsMatch"_sd,                           EFn::kFtsMatch},
    {"generateCheapSortKey"_sd,               EFn::kGenerateCheapSortKey},
    {"generateSortKey"_sd,                    EFn::kGenerateSortKey},
    {"getArraySize"_sd,                       EFn::kGetArraySize},
    {"getElement"_sd,                         EFn::kGetElement},
    {"getField"_sd,                           EFn::kGetField},
    {"getFieldOrElement"_sd,                  EFn::kGetFieldOrElement},
    {"getNonLeafSortKeyAsc"_sd,               EFn::kGetNonLeafSortKeyAsc},
    {"getNonLeafSortKeyDesc"_sd,              EFn::kGetNonLeafSortKeyDesc},
    {"getParam"_sd,                           EFn::kGetParam},
    {"getRegexFlags"_sd,                      EFn::kGetRegexFlags},
    {"getRegexPattern"_sd,                    EFn::kGetRegexPattern},
    {"getSortKeyAsc"_sd,                      EFn::kGetSortKeyAsc},
    {"getSortKeyDesc"_sd,                     EFn::kGetSortKeyDesc},
    {"hasNullBytes"_sd,                       EFn::kHasNullBytes},
    {"hash"_sd,                               EFn::kHash},
    {"hour"_sd,                               EFn::kHour},
    {"indexOfBytes"_sd,                       EFn::kIndexOfBytes},
    {"indexOfCP"_sd,                          EFn::kIndexOfCP},
    {"isArray"_sd,                            EFn::kIsArray},
    {"isArrayEmpty"_sd,                       EFn::kIsArrayEmpty},
    {"isBinData"_sd,                          EFn::kIsBinData},
    {"isDate"_sd,                             EFn::kIsDate},
    {"isDayOfWeek"_sd,                        EFn::kIsDayOfWeek},
    {"isInList"_sd,                           EFn::kIsInList},
    {"isInfinity"_sd,                         EFn::kIsInfinity},
    {"isKeyString"_sd,                        EFn::kIsKeyString},
    {"isMaxKey"_sd,                           EFn::kIsMaxKey},
    {"isMember"_sd,                           EFn::kIsMember},
    {"isMinKey"_sd,                           EFn::kIsMinKey},
    {"isNaN"_sd,                              EFn::kIsNaN},
    {"isNull"_sd,                             EFn::kIsNull},
    {"isNumber"_sd,                           EFn::kIsNumber},
    {"isObject"_sd,                           EFn::kIsObject},
    {"isRecordId"_sd,                         EFn::kIsRecordId},
    {"isString"_sd,                           EFn::kIsString},
    {"isTimeUnit"_sd,                         EFn::kIsTimeUnit},
    {"isTimestamp"_sd,                        EFn::kIsTimestamp},
    {"isTimezone"_sd,                         EFn::kIsTimezone},
    {"isValidToStringFormat"_sd,              EFn::kIsValidToStringFormat},
    {"isoDateToParts"_sd,                     EFn::kIsoDateToParts},
    {"isoDayOfWeek"_sd,                       EFn::kIsoDayOfWeek},
    {"isoWeek"_sd,                            EFn::kIsoWeek},
    {"isoWeekYear"_sd,                        EFn::kIsoWeekYear},
    {"keepFields"_sd,                         EFn::kKeepFields},
    {"ks"_sd,                                 EFn::kKs},
    {"last"_sd,                               EFn::kLast},
    {"ln"_sd,                                 EFn::kLn},
    {"log10"_sd,                              EFn::kLog10},
    {"ltrim"_sd,                              EFn::kLtrim},
    {"magicTraverseF"_sd,                     EFn::kMagicTraverseF},
    {"makeBsonObj"_sd,                        EFn::kMakeBsonObj},
    {"makeObj"_sd,                            EFn::kMakeObj},
    {"makeOwn"_sd,                            EFn::kMakeOwn},
    {"max"_sd,                                EFn::kMax},
    {"maxOfArray"_sd,                         EFn::kMaxOfArray},
    {"mergeObjects"_sd,                       EFn::kMergeObjects},
    {"millisecond"_sd,                        EFn::kMillisecond},
    {"min"_sd,                                EFn::kMin},
    {"minOfArray"_sd,                         EFn::kMinOfArray},
    {"minute"_sd,                             EFn::kMinute},
    {"mod"_sd,                                EFn::kMod},
    {"month"_sd,                              EFn::kMonth},
    {"newArray"_sd,                           EFn::kNewArray},
    {"newArrayFromRange"_sd,                  EFn::kNewArrayFromRange},
    {"newBsonObj"_sd,                         EFn::kNewBsonObj},
    {"newObj"_sd,                             EFn::kNewObj},
    {"objectToArray"_sd,                      EFn::kObjectToArray},
    {"pow"_sd,                                EFn::kPow},
    {"radiansToDegrees"_sd,                   EFn::kRadiansToDegrees},
    {"rand"_sd,                               EFn::kRand},
    {"regexCompile"_sd,                       EFn::kRegexCompile},
    {"regexFind"_sd,                          EFn::kRegexFind},
    {"regexFindAll"_sd,                       EFn::kRegexFindAll},
    {"regexMatch"_sd,                         EFn::kRegexMatch},
    {"replaceOne"_sd,                         EFn::kReplaceOne},
    {"reverseArray"_sd,                       EFn::kReverseArray},
    {"round"_sd,                              EFn::kRound},
    {"rtrim"_sd,                              EFn::kRtrim},
    {"runJsPredicate"_sd,                     EFn::kRunJsPredicate},
    {"second"_sd,                             EFn::kSecond},
    {"setDifference"_sd,                      EFn::kSetDifference},
    {"setEquals"_sd,                          EFn::kSetEquals},
    {"setField"_sd,                           EFn::kSetField},
    {"setIntersection"_sd,                    EFn::kSetIntersection},
    {"setIsSubset"_sd,                        EFn::kSetIsSubset},
    {"setToArray"_sd,                         EFn::kSetToArray},
    {"setUnion"_sd,                           EFn::kSetUnion},
    {"setUnionCapped"_sd,                     EFn::kSetUnionCapped},
    {"shardFilter"_sd,                        EFn::kShardFilter},
    {"shardHash"_sd,                          EFn::kShardHash},
    {"sin"_sd,                                EFn::kSin},
    {"sinh"_sd,                               EFn::kSinh},
    {"sortArray"_sd,                          EFn::kSortArray},
    {"sortKeyComponentVectorGetElement"_sd,   EFn::kSortKeyComponentVectorGetElement},
    {"sortKeyComponentVectorToArray"_sd,      EFn::kSortKeyComponentVectorToArray},
    {"split"_sd,                              EFn::kSplit},
    {"sqrt"_sd,                               EFn::kSqrt},
    {"stdDevPop"_sd,                          EFn::kStdDevPop},
    {"stdDevPopFinalize"_sd,                  EFn::kStdDevPopFinalize},
    {"stdDevSamp"_sd,                         EFn::kStdDevSamp},
    {"stdDevSampFinalize"_sd,                 EFn::kStdDevSampFinalize},
    {"strLenBytes"_sd,                        EFn::kStrLenBytes},
    {"strLenCP"_sd,                           EFn::kStrLenCP},
    {"substrBytes"_sd,                        EFn::kSubstrBytes},
    {"substrCP"_sd,                           EFn::kSubstrCP},
    {"sum"_sd,                                EFn::kSum},
    {"sumOfArray"_sd,                         EFn::kSumOfArray},
    {"tan"_sd,                                EFn::kTan},
    {"tanh"_sd,                               EFn::kTanh},
    {"toLower"_sd,                            EFn::kToLower},
    {"toUpper"_sd,                            EFn::kToUpper},
    {"top"_sd,                                EFn::kTop},
    {"topN"_sd,                               EFn::kTopN},
    {"traverseF"_sd,                          EFn::kTraverseF},
    {"traverseP"_sd,                          EFn::kTraverseP},
    {"trim"_sd,                               EFn::kTrim},
    {"trunc"_sd,                              EFn::kTrunc},
    {"tsIncrement"_sd,                        EFn::kTsIncrement},
    {"tsSecond"_sd,                           EFn::kTsSecond},
    {"typeMatch"_sd,                          EFn::kTypeMatch},
    {"unwindArray"_sd,                        EFn::kUnwindArray},
    {"validateFromStringFormat"_sd,           EFn::kValidateFromStringFormat},
    {"valueBlockAdd"_sd,                      EFn::kValueBlockAdd},
    {"valueBlockAggBottomN"_sd,               EFn::kValueBlockAggBottomN},
    {"valueBlockAggBottomNArray"_sd,          EFn::kValueBlockAggBottomNArray},
    {"valueBlockAggCount"_sd,                 EFn::kValueBlockAggCount},
    {"valueBlockAggDoubleDoubleSum"_sd,       EFn::kValueBlockAggDoubleDoubleSum},
    {"valueBlockAggMax"_sd,                   EFn::kValueBlockAggMax},
    {"valueBlockAggMin"_sd,                   EFn::kValueBlockAggMin},
    {"valueBlockAggSum"_sd,                   EFn::kValueBlockAggSum},
    {"valueBlockAggTopN"_sd,                  EFn::kValueBlockAggTopN},
    {"valueBlockAggTopNArray"_sd,             EFn::kValueBlockAggTopNArray},
    {"valueBlockApplyLambda"_sd,              EFn::kValueBlockApplyLambda},
    {"valueBlockCmp3wScalar"_sd,              EFn::kValueBlockCmp3wScalar},
    {"valueBlockCoerceToBool"_sd,             EFn::kValueBlockCoerceToBool},
    {"valueBlockCombine"_sd,                  EFn::kValueBlockCombine},
    {"valueBlockConvert"_sd,                  EFn::kValueBlockConvert},
    {"valueBlockDateAdd"_sd,                  EFn::kValueBlockDateAdd},
    {"valueBlockDateDiff"_sd,                 EFn::kValueBlockDateDiff},
    {"valueBlockDateTrunc"_sd,                EFn::kValueBlockDateTrunc},
    {"valueBlockDiv"_sd,                      EFn::kValueBlockDiv},
    {"valueBlockEqScalar"_sd,                 EFn::kValueBlockEqScalar},
    {"valueBlockExists"_sd,                   EFn::kValueBlockExists},
    {"valueBlockFillEmpty"_sd,                EFn::kValueBlockFillEmpty},
    {"valueBlockFillEmptyBlock"_sd,           EFn::kValueBlockFillEmptyBlock},
    {"valueBlockFillType"_sd,                 EFn::kValueBlockFillType},
    {"valueBlockGetSortKeyAsc"_sd,            EFn::kValueBlockGetSortKeyAsc},
    {"valueBlockGetSortKeyDesc"_sd,           EFn::kValueBlockGetSortKeyDesc},
    {"valueBlockGtScalar"_sd,                 EFn::kValueBlockGtScalar},
    {"valueBlockGteScalar"_sd,                EFn::kValueBlockGteScalar},
    {"valueBlockIsMember"_sd,                 EFn::kValueBlockIsMember},
    {"valueBlockIsTimezone"_sd,               EFn::kValueBlockIsTimezone},
    {"valueBlockLogicalAnd"_sd,               EFn::kValueBlockLogicalAnd},
    {"valueBlockLogicalNot"_sd,               EFn::kValueBlockLogicalNot},
    {"valueBlockLogicalOr"_sd,                EFn::kValueBlockLogicalOr},
    {"valueBlockLtScalar"_sd,                 EFn::kValueBlockLtScalar},
    {"valueBlockLteScalar"_sd,                EFn::kValueBlockLteScalar},
    {"valueBlockMod"_sd,                      EFn::kValueBlockMod},
    {"valueBlockMult"_sd,                     EFn::kValueBlockMult},
    {"valueBlockNeqScalar"_sd,                EFn::kValueBlockNeqScalar},
    {"valueBlockNewFill"_sd,                  EFn::kValueBlockNewFill},
    {"valueBlockNone"_sd,                     EFn::kValueBlockNone},
    {"valueBlockRound"_sd,                    EFn::kValueBlockRound},
    {"valueBlockSize"_sd,                     EFn::kValueBlockSize},
    {"valueBlockSub"_sd,                      EFn::kValueBlockSub},
    {"valueBlockTrunc"_sd,                    EFn::kValueBlockTrunc},
    {"valueBlockTypeMatch"_sd,                EFn::kValueBlockTypeMatch},
    {"week"_sd,                               EFn::kWeek},
    {"year"_sd,                               EFn::kYear},
    {"zipArrays"_sd,                          EFn::kZipArrays},
};
// clang-format on

static_assert(std::size(kEFnByName) == static_cast<size_t>(EFn::kNumFunctions),
              "kEFnByName length must equal EFn::kNumFunctions");

// Aliases for dollar-prefixed MQL accumulator names for backward compatibility.
static constexpr EFnEntry kEFnAliases[] = {
    {"$addToSet"_sd, EFn::kAddToSet},
    {"$first"_sd, EFn::kFirst},
    {"$last"_sd, EFn::kLast},
    {"$max"_sd, EFn::kMax},
    {"$min"_sd, EFn::kMin},
    {"$push"_sd, EFn::kAddToArray},
    {"$sum"_sd, EFn::kSum},
};

static_assert(std::is_sorted(std::begin(kEFnByName),
                             std::end(kEFnByName),
                             [](const EFnEntry& a, const EFnEntry& b) { return a.name < b.name; }),
              "kEFnByName must be sorted by name");
static_assert(std::is_sorted(std::begin(kEFnAliases),
                             std::end(kEFnAliases),
                             [](const EFnEntry& a, const EFnEntry& b) { return a.name < b.name; }),
              "kEFnAliases must be sorted by name");
// toString() uses the enum's integer as a direct array index, so position i must hold the
// entry for EFn(i). Verified by requiring kEFnByName[i].value == EFn(i) for all i.
static_assert(
    []() constexpr {
        for (size_t i = 0; i < static_cast<size_t>(EFn::kNumFunctions); ++i) {
            if (kEFnByName[i].value != static_cast<EFn>(i))
                return false;
        }
        return true;
    }(),
    "kEFnByName[i].value must equal static_cast<EFn>(i) for all i; toString() relies on "
    "this");

}  // namespace

// ---------------------------------------------------------------------------
// toString
// ---------------------------------------------------------------------------

StringData toString(EFn fn) {
    auto idx = static_cast<size_t>(fn);
    tassert(8026701,
            "toString called with out-of-range EFn value",
            idx < static_cast<size_t>(EFn::kNumFunctions));
    return kEFnByName[idx].name;
}

// ---------------------------------------------------------------------------
// fromString
// ---------------------------------------------------------------------------

EFn fromString(StringData name) {
    // Check the primary (canonical) table first via binary search.
    auto it = std::lower_bound(std::begin(kEFnByName),
                               std::end(kEFnByName),
                               name,
                               [](const EFnEntry& entry, StringData n) { return entry.name < n; });
    if (it != std::end(kEFnByName) && it->name == name) {
        return it->value;
    }

    // Fall back to the alias table (also sorted, also binary-searched).
    auto ait = std::lower_bound(std::begin(kEFnAliases),
                                std::end(kEFnAliases),
                                name,
                                [](const EFnEntry& entry, StringData n) { return entry.name < n; });
    if (ait != std::end(kEFnAliases) && ait->name == name) {
        return ait->value;
    }

    tasserted(8026700, str::stream() << "fromString: unknown SBE function name '" << name << "'");
}

}  // namespace mongo::sbe
