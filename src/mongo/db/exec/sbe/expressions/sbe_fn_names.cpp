// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/expressions/sbe_fn_names.h"

#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <array>
#include <string_view>
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
using namespace std::literals::string_view_literals;

struct EFnEntry {
    std::string_view name;
    EFn value;
};

// Sorted by name (same alphabetical order as the enum values).
// Also used by toString(): kEFnByName[static_cast<size_t>(fn)].name gives the string.
// clang-format off
static constexpr EFnEntry kEFnByName[] = {
    {"abs"sv,                                EFn::kAbs},
    {"acos"sv,                               EFn::kAcos},
    {"acosh"sv,                              EFn::kAcosh},
    {"addToArray"sv,                         EFn::kAddToArray},
    {"addToArrayCapped"sv,                   EFn::kAddToArrayCapped},
    {"addToSet"sv,                           EFn::kAddToSet},
    {"addToSetCapped"sv,                     EFn::kAddToSetCapped},
    {"aggBottomN"sv,                         EFn::kAggBottomN},
    {"aggBottomNArray"sv,                    EFn::kAggBottomNArray},
    {"aggBottomNFinalize"sv,                 EFn::kAggBottomNFinalize},
    {"aggBottomNMerge"sv,                    EFn::kAggBottomNMerge},
    {"aggCollSetUnion"sv,                    EFn::kAggCollSetUnion},
    {"aggCollSetUnionCapped"sv,              EFn::kAggCollSetUnionCapped},
    {"aggConcatArraysCapped"sv,              EFn::kAggConcatArraysCapped},
    {"aggCovarianceAdd"sv,                   EFn::kAggCovarianceAdd},
    {"aggCovariancePopFinalize"sv,           EFn::kAggCovariancePopFinalize},
    {"aggCovarianceRemove"sv,                EFn::kAggCovarianceRemove},
    {"aggCovarianceSampFinalize"sv,          EFn::kAggCovarianceSampFinalize},
    {"aggDenseRank"sv,                       EFn::kAggDenseRank},
    {"aggDenseRankColl"sv,                   EFn::kAggDenseRankColl},
    {"aggDerivativeFinalize"sv,              EFn::kAggDerivativeFinalize},
    {"aggDoubleDoubleSum"sv,                 EFn::kAggDoubleDoubleSum},
    {"aggExpMovingAvg"sv,                    EFn::kAggExpMovingAvg},
    {"aggExpMovingAvgFinalize"sv,            EFn::kAggExpMovingAvgFinalize},
    {"aggFirstN"sv,                          EFn::kAggFirstN},
    {"aggFirstNFinalize"sv,                  EFn::kAggFirstNFinalize},
    {"aggFirstNMerge"sv,                     EFn::kAggFirstNMerge},
    {"aggFirstNNeedsMoreInput"sv,            EFn::kAggFirstNNeedsMoreInput},
    {"aggIntegralAdd"sv,                     EFn::kAggIntegralAdd},
    {"aggIntegralFinalize"sv,                EFn::kAggIntegralFinalize},
    {"aggIntegralInit"sv,                    EFn::kAggIntegralInit},
    {"aggIntegralRemove"sv,                  EFn::kAggIntegralRemove},
    {"aggLastN"sv,                           EFn::kAggLastN},
    {"aggLastNFinalize"sv,                   EFn::kAggLastNFinalize},
    {"aggLastNMerge"sv,                      EFn::kAggLastNMerge},
    {"aggLinearFillAdd"sv,                   EFn::kAggLinearFillAdd},
    {"aggLinearFillCanAdd"sv,                EFn::kAggLinearFillCanAdd},
    {"aggLinearFillFinalize"sv,              EFn::kAggLinearFillFinalize},
    {"aggMaxN"sv,                            EFn::kAggMaxN},
    {"aggMaxNFinalize"sv,                    EFn::kAggMaxNFinalize},
    {"aggMaxNMerge"sv,                       EFn::kAggMaxNMerge},
    {"aggMergeDoubleDoubleSums"sv,           EFn::kAggMergeDoubleDoubleSums},
    {"aggMergeStdDevs"sv,                    EFn::kAggMergeStdDevs},
    {"aggMinN"sv,                            EFn::kAggMinN},
    {"aggMinNFinalize"sv,                    EFn::kAggMinNFinalize},
    {"aggMinNMerge"sv,                       EFn::kAggMinNMerge},
    {"aggRank"sv,                            EFn::kAggRank},
    {"aggRankColl"sv,                        EFn::kAggRankColl},
    {"aggRankFinalize"sv,                    EFn::kAggRankFinalize},
    {"aggRemovableAddToSetAdd"sv,            EFn::kAggRemovableAddToSetAdd},
    {"aggRemovableAddToSetRemove"sv,         EFn::kAggRemovableAddToSetRemove},
    {"aggRemovableAvgFinalize"sv,            EFn::kAggRemovableAvgFinalize},
    {"aggRemovableBottomNAdd"sv,             EFn::kAggRemovableBottomNAdd},
    {"aggRemovableBottomNFinalize"sv,        EFn::kAggRemovableBottomNFinalize},
    {"aggRemovableBottomNInit"sv,            EFn::kAggRemovableBottomNInit},
    {"aggRemovableBottomNRemove"sv,          EFn::kAggRemovableBottomNRemove},
    {"aggRemovableConcatArraysAdd"sv,        EFn::kAggRemovableConcatArraysAdd},
    {"aggRemovableConcatArraysFinalize"sv,   EFn::kAggRemovableConcatArraysFinalize},
    {"aggRemovableConcatArraysInit"sv,       EFn::kAggRemovableConcatArraysInit},
    {"aggRemovableConcatArraysRemove"sv,     EFn::kAggRemovableConcatArraysRemove},
    {"aggRemovableFirstNAdd"sv,              EFn::kAggRemovableFirstNAdd},
    {"aggRemovableFirstNFinalize"sv,         EFn::kAggRemovableFirstNFinalize},
    {"aggRemovableFirstNInit"sv,             EFn::kAggRemovableFirstNInit},
    {"aggRemovableFirstNRemove"sv,           EFn::kAggRemovableFirstNRemove},
    {"aggRemovableLastNAdd"sv,               EFn::kAggRemovableLastNAdd},
    {"aggRemovableLastNFinalize"sv,          EFn::kAggRemovableLastNFinalize},
    {"aggRemovableLastNInit"sv,              EFn::kAggRemovableLastNInit},
    {"aggRemovableLastNRemove"sv,            EFn::kAggRemovableLastNRemove},
    {"aggRemovableMaxNFinalize"sv,           EFn::kAggRemovableMaxNFinalize},
    {"aggRemovableMinMaxNAdd"sv,             EFn::kAggRemovableMinMaxNAdd},
    {"aggRemovableMinMaxNCollInit"sv,        EFn::kAggRemovableMinMaxNCollInit},
    {"aggRemovableMinMaxNInit"sv,            EFn::kAggRemovableMinMaxNInit},
    {"aggRemovableMinMaxNRemove"sv,          EFn::kAggRemovableMinMaxNRemove},
    {"aggRemovableMinNFinalize"sv,           EFn::kAggRemovableMinNFinalize},
    {"aggRemovablePushAdd"sv,                EFn::kAggRemovablePushAdd},
    {"aggRemovablePushFinalize"sv,           EFn::kAggRemovablePushFinalize},
    {"aggRemovablePushRemove"sv,             EFn::kAggRemovablePushRemove},
    {"aggRemovableSetCommonCollInit"sv,      EFn::kAggRemovableSetCommonCollInit},
    {"aggRemovableSetCommonFinalize"sv,      EFn::kAggRemovableSetCommonFinalize},
    {"aggRemovableSetCommonInit"sv,          EFn::kAggRemovableSetCommonInit},
    {"aggRemovableSetUnionAdd"sv,            EFn::kAggRemovableSetUnionAdd},
    {"aggRemovableSetUnionRemove"sv,         EFn::kAggRemovableSetUnionRemove},
    {"aggRemovableStdDevAdd"sv,              EFn::kAggRemovableStdDevAdd},
    {"aggRemovableStdDevPopFinalize"sv,      EFn::kAggRemovableStdDevPopFinalize},
    {"aggRemovableStdDevRemove"sv,           EFn::kAggRemovableStdDevRemove},
    {"aggRemovableStdDevSampFinalize"sv,     EFn::kAggRemovableStdDevSampFinalize},
    {"aggRemovableSumAdd"sv,                 EFn::kAggRemovableSumAdd},
    {"aggRemovableSumFinalize"sv,            EFn::kAggRemovableSumFinalize},
    {"aggRemovableSumRemove"sv,              EFn::kAggRemovableSumRemove},
    {"aggRemovableTopNAdd"sv,                EFn::kAggRemovableTopNAdd},
    {"aggRemovableTopNFinalize"sv,           EFn::kAggRemovableTopNFinalize},
    {"aggRemovableTopNInit"sv,               EFn::kAggRemovableTopNInit},
    {"aggRemovableTopNRemove"sv,             EFn::kAggRemovableTopNRemove},
    {"aggSetUnion"sv,                        EFn::kAggSetUnion},
    {"aggSetUnionCapped"sv,                  EFn::kAggSetUnionCapped},
    {"aggState"sv,                           EFn::kAggState},
    {"aggStdDev"sv,                          EFn::kAggStdDev},
    {"aggTopN"sv,                            EFn::kAggTopN},
    {"aggTopNArray"sv,                       EFn::kAggTopNArray},
    {"aggTopNFinalize"sv,                    EFn::kAggTopNFinalize},
    {"aggTopNMerge"sv,                       EFn::kAggTopNMerge},
    {"array"sv,                              EFn::kArray},
    {"arrayToObject"sv,                      EFn::kArrayToObject},
    {"arrayToSet"sv,                         EFn::kArrayToSet},
    {"asin"sv,                               EFn::kAsin},
    {"asinh"sv,                              EFn::kAsinh},
    {"atan"sv,                               EFn::kAtan},
    {"atan2"sv,                              EFn::kAtan2},
    {"atanh"sv,                              EFn::kAtanh},
    {"bitTestMask"sv,                        EFn::kBitTestMask},
    {"bitTestPosition"sv,                    EFn::kBitTestPosition},
    {"bitTestZero"sv,                        EFn::kBitTestZero},
    {"blockTraverseFPlaceholder"sv,          EFn::kBlockTraverseFPlaceholder},
    {"bottom"sv,                             EFn::kBottom},
    {"bottomN"sv,                            EFn::kBottomN},
    {"bsonSize"sv,                           EFn::kBsonSize},
    {"ceil"sv,                               EFn::kCeil},
    {"cellBlockGetFlatValuesBlock"sv,        EFn::kCellBlockGetFlatValuesBlock},
    {"cellFoldValues_F"sv,                   EFn::kCellFoldValues_F},
    {"cellFoldValues_P"sv,                   EFn::kCellFoldValues_P},
    {"coerceToBool"sv,                       EFn::kCoerceToBool},
    {"coerceToString"sv,                     EFn::kCoerceToString},
    {"collAddToSet"sv,                       EFn::kCollAddToSet},
    {"collAddToSetCapped"sv,                 EFn::kCollAddToSetCapped},
    {"collArrayToSet"sv,                     EFn::kCollArrayToSet},
    {"collComparisonKey"sv,                  EFn::kCollComparisonKey},
    {"collIsMember"sv,                       EFn::kCollIsMember},
    {"collKs"sv,                             EFn::kCollKs},
    {"collMax"sv,                            EFn::kCollMax},
    {"collMin"sv,                            EFn::kCollMin},
    {"collSetDifference"sv,                  EFn::kCollSetDifference},
    {"collSetEquals"sv,                      EFn::kCollSetEquals},
    {"collSetIntersection"sv,                EFn::kCollSetIntersection},
    {"collSetIsSubset"sv,                    EFn::kCollSetIsSubset},
    {"collSetUnion"sv,                       EFn::kCollSetUnion},
    {"collSetUnionCapped"sv,                 EFn::kCollSetUnionCapped},
    {"concat"sv,                             EFn::kConcat},
    {"concatArrays"sv,                       EFn::kConcatArrays},
    {"concatArraysCapped"sv,                 EFn::kConcatArraysCapped},
    {"convert"sv,                            EFn::kConvert},
    {"convertSimpleSumToDoubleDoubleSum"sv,  EFn::kConvertSimpleSumToDoubleDoubleSum},
    {"cos"sv,                                EFn::kCos},
    {"cosh"sv,                               EFn::kCosh},
    {"count"sv,                              EFn::kCount},
    {"currentDate"sv,                        EFn::kCurrentDate},
    {"dateAdd"sv,                            EFn::kDateAdd},
    {"dateDiff"sv,                           EFn::kDateDiff},
    {"dateFromString"sv,                     EFn::kDateFromString},
    {"dateFromStringNoThrow"sv,              EFn::kDateFromStringNoThrow},
    {"dateParts"sv,                          EFn::kDateParts},
    {"datePartsWeekYear"sv,                  EFn::kDatePartsWeekYear},
    {"dateToParts"sv,                        EFn::kDateToParts},
    {"dateToString"sv,                       EFn::kDateToString},
    {"dateTrunc"sv,                          EFn::kDateTrunc},
    {"dayOfMonth"sv,                         EFn::kDayOfMonth},
    {"dayOfWeek"sv,                          EFn::kDayOfWeek},
    {"dayOfYear"sv,                          EFn::kDayOfYear},
    {"degreesToRadians"sv,                   EFn::kDegreesToRadians},
    {"doubleDoublePartialSumFinalize"sv,     EFn::kDoubleDoublePartialSumFinalize},
    {"doubleDoubleSum"sv,                    EFn::kDoubleDoubleSum},
    {"doubleDoubleSumFinalize"sv,            EFn::kDoubleDoubleSumFinalize},
    {"dropFields"sv,                         EFn::kDropFields},
    {"exists"sv,                             EFn::kExists},
    {"exp"sv,                                EFn::kExp},
    {"extractSubArray"sv,                    EFn::kExtractSubArray},
    {"fail"sv,                               EFn::kFail},
    {"fillType"sv,                           EFn::kFillType},
    {"first"sv,                              EFn::kFirst},
    {"floor"sv,                              EFn::kFloor},
    {"ftsMatch"sv,                           EFn::kFtsMatch},
    {"generateCheapSortKey"sv,               EFn::kGenerateCheapSortKey},
    {"generateSortKey"sv,                    EFn::kGenerateSortKey},
    {"getArraySize"sv,                       EFn::kGetArraySize},
    {"getElement"sv,                         EFn::kGetElement},
    {"getField"sv,                           EFn::kGetField},
    {"getFieldOrElement"sv,                  EFn::kGetFieldOrElement},
    {"getNonLeafSortKeyAsc"sv,               EFn::kGetNonLeafSortKeyAsc},
    {"getNonLeafSortKeyDesc"sv,              EFn::kGetNonLeafSortKeyDesc},
    {"getParam"sv,                           EFn::kGetParam},
    {"getRegexFlags"sv,                      EFn::kGetRegexFlags},
    {"getRegexPattern"sv,                    EFn::kGetRegexPattern},
    {"getSortKeyAsc"sv,                      EFn::kGetSortKeyAsc},
    {"getSortKeyDesc"sv,                     EFn::kGetSortKeyDesc},
    {"hasNullBytes"sv,                       EFn::kHasNullBytes},
    {"hash"sv,                               EFn::kHash},
    {"hour"sv,                               EFn::kHour},
    {"indexOfBytes"sv,                       EFn::kIndexOfBytes},
    {"indexOfCP"sv,                          EFn::kIndexOfCP},
    {"isArray"sv,                            EFn::kIsArray},
    {"isArrayEmpty"sv,                       EFn::kIsArrayEmpty},
    {"isBinData"sv,                          EFn::kIsBinData},
    {"isDate"sv,                             EFn::kIsDate},
    {"isDayOfWeek"sv,                        EFn::kIsDayOfWeek},
    {"isInList"sv,                           EFn::kIsInList},
    {"isInfinity"sv,                         EFn::kIsInfinity},
    {"isKeyString"sv,                        EFn::kIsKeyString},
    {"isMaxKey"sv,                           EFn::kIsMaxKey},
    {"isMember"sv,                           EFn::kIsMember},
    {"isMinKey"sv,                           EFn::kIsMinKey},
    {"isNaN"sv,                              EFn::kIsNaN},
    {"isNull"sv,                             EFn::kIsNull},
    {"isNullish"sv,                          EFn::kIsNullish},
    {"isNumber"sv,                           EFn::kIsNumber},
    {"isObject"sv,                           EFn::kIsObject},
    {"isRecordId"sv,                         EFn::kIsRecordId},
    {"isString"sv,                           EFn::kIsString},
    {"isTimeUnit"sv,                         EFn::kIsTimeUnit},
    {"isTimestamp"sv,                        EFn::kIsTimestamp},
    {"isTimezone"sv,                         EFn::kIsTimezone},
    {"isValidToStringFormat"sv,              EFn::kIsValidToStringFormat},
    {"isoDateToParts"sv,                     EFn::kIsoDateToParts},
    {"isoDayOfWeek"sv,                       EFn::kIsoDayOfWeek},
    {"isoWeek"sv,                            EFn::kIsoWeek},
    {"isoWeekYear"sv,                        EFn::kIsoWeekYear},
    {"keepFields"sv,                         EFn::kKeepFields},
    {"ks"sv,                                 EFn::kKs},
    {"last"sv,                               EFn::kLast},
    {"ln"sv,                                 EFn::kLn},
    {"log10"sv,                              EFn::kLog10},
    {"ltrim"sv,                              EFn::kLtrim},
    {"makeBsonObj"sv,                        EFn::kMakeBsonObj},
    {"makeObj"sv,                            EFn::kMakeObj},
    {"makeOwn"sv,                            EFn::kMakeOwn},
    {"max"sv,                                EFn::kMax},
    {"mergeObjects"sv,                       EFn::kMergeObjects},
    {"millisecond"sv,                        EFn::kMillisecond},
    {"min"sv,                                EFn::kMin},
    {"minute"sv,                             EFn::kMinute},
    {"mod"sv,                                EFn::kMod},
    {"month"sv,                              EFn::kMonth},
    {"newArray"sv,                           EFn::kNewArray},
    {"newArrayFromRange"sv,                  EFn::kNewArrayFromRange},
    {"newBsonObj"sv,                         EFn::kNewBsonObj},
    {"newObj"sv,                             EFn::kNewObj},
    {"objectToArray"sv,                      EFn::kObjectToArray},
    {"pow"sv,                                EFn::kPow},
    {"radiansToDegrees"sv,                   EFn::kRadiansToDegrees},
    {"rand"sv,                               EFn::kRand},
    {"regexCompile"sv,                       EFn::kRegexCompile},
    {"regexFind"sv,                          EFn::kRegexFind},
    {"regexFindAll"sv,                       EFn::kRegexFindAll},
    {"regexMatch"sv,                         EFn::kRegexMatch},
    {"replaceOne"sv,                         EFn::kReplaceOne},
    {"reverseArray"sv,                       EFn::kReverseArray},
    {"round"sv,                              EFn::kRound},
    {"rtrim"sv,                              EFn::kRtrim},
    {"runJsPredicate"sv,                     EFn::kRunJsPredicate},
    {"second"sv,                             EFn::kSecond},
    {"setDifference"sv,                      EFn::kSetDifference},
    {"setEquals"sv,                          EFn::kSetEquals},
    {"setIntersection"sv,                    EFn::kSetIntersection},
    {"setIsSubset"sv,                        EFn::kSetIsSubset},
    {"setToArray"sv,                         EFn::kSetToArray},
    {"setUnion"sv,                           EFn::kSetUnion},
    {"setUnionCapped"sv,                     EFn::kSetUnionCapped},
    {"shardFilter"sv,                        EFn::kShardFilter},
    {"shardHash"sv,                          EFn::kShardHash},
    {"sin"sv,                                EFn::kSin},
    {"sinh"sv,                               EFn::kSinh},
    {"sortArray"sv,                          EFn::kSortArray},
    {"sortKeyComponentVectorGetElement"sv,   EFn::kSortKeyComponentVectorGetElement},
    {"sortKeyComponentVectorToArray"sv,      EFn::kSortKeyComponentVectorToArray},
    {"split"sv,                              EFn::kSplit},
    {"sqrt"sv,                               EFn::kSqrt},
    {"stdDevPopFinalize"sv,                  EFn::kStdDevPopFinalize},
    {"stdDevSampFinalize"sv,                 EFn::kStdDevSampFinalize},
    {"strLenBytes"sv,                        EFn::kStrLenBytes},
    {"strLenCP"sv,                           EFn::kStrLenCP},
    {"substrBytes"sv,                        EFn::kSubstrBytes},
    {"substrCP"sv,                           EFn::kSubstrCP},
    {"sum"sv,                                EFn::kSum},
    {"tan"sv,                                EFn::kTan},
    {"tanh"sv,                               EFn::kTanh},
    {"toLower"sv,                            EFn::kToLower},
    {"toUpper"sv,                            EFn::kToUpper},
    {"top"sv,                                EFn::kTop},
    {"topN"sv,                               EFn::kTopN},
    {"traverseF"sv,                          EFn::kTraverseF},
    {"traverseP"sv,                          EFn::kTraverseP},
    {"trim"sv,                               EFn::kTrim},
    {"trunc"sv,                              EFn::kTrunc},
    {"tsIncrement"sv,                        EFn::kTsIncrement},
    {"tsSecond"sv,                           EFn::kTsSecond},
    {"typeMatch"sv,                          EFn::kTypeMatch},
    {"unwindArray"sv,                        EFn::kUnwindArray},
    {"validateFromStringFormat"sv,           EFn::kValidateFromStringFormat},
    {"valueBlockAdd"sv,                      EFn::kValueBlockAdd},
    {"valueBlockAggBottomN"sv,               EFn::kValueBlockAggBottomN},
    {"valueBlockAggBottomNArray"sv,          EFn::kValueBlockAggBottomNArray},
    {"valueBlockAggCount"sv,                 EFn::kValueBlockAggCount},
    {"valueBlockAggDoubleDoubleSum"sv,       EFn::kValueBlockAggDoubleDoubleSum},
    {"valueBlockAggMax"sv,                   EFn::kValueBlockAggMax},
    {"valueBlockAggMin"sv,                   EFn::kValueBlockAggMin},
    {"valueBlockAggSum"sv,                   EFn::kValueBlockAggSum},
    {"valueBlockAggTopN"sv,                  EFn::kValueBlockAggTopN},
    {"valueBlockAggTopNArray"sv,             EFn::kValueBlockAggTopNArray},
    {"valueBlockApplyLambda"sv,              EFn::kValueBlockApplyLambda},
    {"valueBlockCmp3wScalar"sv,              EFn::kValueBlockCmp3wScalar},
    {"valueBlockCoerceToBool"sv,             EFn::kValueBlockCoerceToBool},
    {"valueBlockCombine"sv,                  EFn::kValueBlockCombine},
    {"valueBlockConvert"sv,                  EFn::kValueBlockConvert},
    {"valueBlockDateAdd"sv,                  EFn::kValueBlockDateAdd},
    {"valueBlockDateDiff"sv,                 EFn::kValueBlockDateDiff},
    {"valueBlockDateTrunc"sv,                EFn::kValueBlockDateTrunc},
    {"valueBlockDiv"sv,                      EFn::kValueBlockDiv},
    {"valueBlockEqScalar"sv,                 EFn::kValueBlockEqScalar},
    {"valueBlockExists"sv,                   EFn::kValueBlockExists},
    {"valueBlockFillEmpty"sv,                EFn::kValueBlockFillEmpty},
    {"valueBlockFillEmptyBlock"sv,           EFn::kValueBlockFillEmptyBlock},
    {"valueBlockFillType"sv,                 EFn::kValueBlockFillType},
    {"valueBlockGetSortKeyAsc"sv,            EFn::kValueBlockGetSortKeyAsc},
    {"valueBlockGetSortKeyDesc"sv,           EFn::kValueBlockGetSortKeyDesc},
    {"valueBlockGtScalar"sv,                 EFn::kValueBlockGtScalar},
    {"valueBlockGteScalar"sv,                EFn::kValueBlockGteScalar},
    {"valueBlockIsMember"sv,                 EFn::kValueBlockIsMember},
    {"valueBlockIsNullish"sv,                EFn::kValueBlockIsNullish},
    {"valueBlockIsTimezone"sv,               EFn::kValueBlockIsTimezone},
    {"valueBlockLogicalAnd"sv,               EFn::kValueBlockLogicalAnd},
    {"valueBlockLogicalNot"sv,               EFn::kValueBlockLogicalNot},
    {"valueBlockLogicalOr"sv,                EFn::kValueBlockLogicalOr},
    {"valueBlockLtScalar"sv,                 EFn::kValueBlockLtScalar},
    {"valueBlockLteScalar"sv,                EFn::kValueBlockLteScalar},
    {"valueBlockMod"sv,                      EFn::kValueBlockMod},
    {"valueBlockMult"sv,                     EFn::kValueBlockMult},
    {"valueBlockNeqScalar"sv,                EFn::kValueBlockNeqScalar},
    {"valueBlockNewFill"sv,                  EFn::kValueBlockNewFill},
    {"valueBlockNone"sv,                     EFn::kValueBlockNone},
    {"valueBlockRound"sv,                    EFn::kValueBlockRound},
    {"valueBlockSize"sv,                     EFn::kValueBlockSize},
    {"valueBlockSub"sv,                      EFn::kValueBlockSub},
    {"valueBlockTrunc"sv,                    EFn::kValueBlockTrunc},
    {"valueBlockTypeMatch"sv,                EFn::kValueBlockTypeMatch},
    {"week"sv,                               EFn::kWeek},
    {"year"sv,                               EFn::kYear},
    {"zipArrays"sv,                          EFn::kZipArrays},
};
// clang-format on

static_assert(std::size(kEFnByName) == static_cast<size_t>(EFn::kNumFunctions),
              "kEFnByName length must equal EFn::kNumFunctions");

// Aliases for dollar-prefixed MQL accumulator names for backward compatibility.
static constexpr EFnEntry kEFnAliases[] = {
    {"$addToSet"sv, EFn::kAddToSet},
    {"$first"sv, EFn::kFirst},
    {"$last"sv, EFn::kLast},
    {"$max"sv, EFn::kMax},
    {"$min"sv, EFn::kMin},
    {"$push"sv, EFn::kAddToArray},
    {"$sum"sv, EFn::kSum},
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

std::string_view toString(EFn fn) {
    auto idx = static_cast<size_t>(fn);
    tassert(8026701,
            "toString called with out-of-range EFn value",
            idx < static_cast<size_t>(EFn::kNumFunctions));
    return kEFnByName[idx].name;
}

// ---------------------------------------------------------------------------
// fromString
// ---------------------------------------------------------------------------

EFn fromString(std::string_view name) {
    // Check the primary (canonical) table first via binary search.
    auto it =
        std::lower_bound(std::begin(kEFnByName),
                         std::end(kEFnByName),
                         name,
                         [](const EFnEntry& entry, std::string_view n) { return entry.name < n; });
    if (it != std::end(kEFnByName) && it->name == name) {
        return it->value;
    }

    // Fall back to the alias table (also sorted, also binary-searched).
    auto ait =
        std::lower_bound(std::begin(kEFnAliases),
                         std::end(kEFnAliases),
                         name,
                         [](const EFnEntry& entry, std::string_view n) { return entry.name < n; });
    if (ait != std::end(kEFnAliases) && ait->name == name) {
        return ait->value;
    }

    tasserted(8026700, str::stream() << "fromString: unknown SBE function name '" << name << "'");
}

}  // namespace mongo::sbe
