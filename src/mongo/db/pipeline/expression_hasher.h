/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/pipeline/accumulator_percentile.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_find_internal.h"
#include "mongo/db/pipeline/expression_function.h"
#include "mongo/db/pipeline/expression_visitor.h"
#include "mongo/db/query/expression_walker.h"
#include "mongo/util/hash_utils.h"

namespace mongo {
template <typename H>
H AbslHashValue(H h, const Value& value) {
    return H::combine(std::move(h), ValueComparator{}.hash(value));
}

template <typename H>
H AbslHashValue(H h, const FieldPath& value) {
    return H::combine(std::move(h), value.fullPath());
}

template <typename H>
H AbslHashValue(H h, const TimeZone& value) {
    return H::combine(std::move(h), value.toString());
}

template <typename H>
class ExpressionHashVisitor : public ExpressionConstVisitor {
public:
    enum class OpType {
        kConst,
        kAbs,
        kAdd,
        kAllElementsTrue,
        kAnd,
        kAnyElementTrue,
        kTestApiVersion,
        kArray,
        kArrayElemAt,
        kBitAnd,
        kBitOr,
        kBitXor,
        kBitNot,
        kFirst,
        kLast,
        kObjectToArray,
        kArrayToObject,
        kBsonSize,
        kCeil,
        kCoerceToBool,
        kCompare,
        kConcat,
        kConcatArrays,
        kCond,
        kDateFromString,
        kDateFromParts,
        kDateDiff,
        kDateToParts,
        kDateToString,
        kDateTrunc,
        kDivide,
        kExp,
        kFieldPath,
        kFilter,
        kFloor,
        kIfNull,
        kIn,
        kIndexOfArray,
        kIndexOfBytes,
        kIndexOfCP,
        kIsNumber,
        kLet,
        kLn,
        kLog,
        kLog10,
        kInternalFLEBetween,
        kInternalFLEEqual,
        kMap,
        kMeta,
        kMod,
        kMultiply,
        kNot,
        kObject,
        kOr,
        kPow,
        kRange,
        kReduce,
        kReplaceOne,
        kReplaceAll,
        kSetDifference,
        kSetEquals,
        kSetIntersection,
        kSetIsSubset,
        kSetUnion,
        kSize,
        kReverseArray,
        kSortArray,
        kSlice,
        kIsArray,
        kInternalFindAllValuesAtPath,
        kRandom,
        kRound,
        kSplit,
        kSqrt,
        kStrcasecmp,
        kSubstrBytes,
        kSubstrCP,
        kStrLenBytes,
        kBinarySize,
        kStrLenCP,
        kSubtract,
        kSwitch,
        kToLower,
        kToUpper,
        kTrim,
        kTrunc,
        kType,
        kZip,
        kConvert,
        kRegexFind,
        kRegexFindAll,
        kRegexMatch,
        kCosine,
        kSine,
        kTangent,
        kArcCosine,
        kArcSine,
        kArcTangent,
        kArcTangent2,
        kHyperbolicArcTangent,
        kHyperbolicArcCosine,
        kHyperbolicArcSine,
        kHyperbolicTangent,
        kHyperbolicCosine,
        kHyperbolicSine,
        kDegreesToRadians,
        kRadiansToDegrees,
        kDayOfMonth,
        kDayOfWeek,
        kDayOfYear,
        kHour,
        kMillisecond,
        kMinute,
        kMonth,
        kSecond,
        kWeek,
        kIsoWeekYear,
        kIsoDayOfWeek,
        kIsoWeek,
        kYear,
        kAccumulatorAvg,
        kAccumulatorFirstN,
        kAccumulatorLastN,
        kAccumulatorMax,
        kAccumulatorMin,
        kAccumulatorMaxN,
        kAccumulatorMinN,
        kAccumulatorMedian,
        kAccumulatorPercentile,
        kAccumulatorStdDevPop,
        kAccumulatorStdDevSamp,
        kAccumulatorSum,
        kAccumulatorMergeObjects,
        kTestable,
        kInternalJsEmit,
        kFunction,
        kInternalFindSlice,
        kInternalFindPositional,
        kInternalFindElemMatch,
        kToHashedIndexKey,
        kDateAdd,
        kDateSubtract,
        kGetField,
        kSetField,
        kTsSecond,
        kTsIncrement,
        kInternalOwningShard,
        kInternalIndexKey,
    };

    explicit ExpressionHashVisitor(H hashState) : _hashState(std::move(hashState)) {}

    void visit(const ExpressionConstant* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kConst, expr->getValue());
    }

    void visit(const ExpressionAbs* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kAbs);
    }

    void visit(const ExpressionAdd* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kAdd);
    }

    void visit(const ExpressionAllElementsTrue* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kAllElementsTrue);
    }

    void visit(const ExpressionAnd* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kAnd);
    }

    void visit(const ExpressionAnyElementTrue* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kAnyElementTrue);
    }

    void visit(const ExpressionTestApiVersion* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kTestApiVersion);
    }

    void visit(const ExpressionArray* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kArray);
    }

    void visit(const ExpressionArrayElemAt* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kArrayElemAt);
    }

    void visit(const ExpressionBitAnd* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kBitAnd);
    }

    void visit(const ExpressionBitOr* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kBitOr);
    }

    void visit(const ExpressionBitXor* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kBitXor);
    }

    void visit(const ExpressionBitNot* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kBitNot);
    }

    void visit(const ExpressionFirst* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kFirst);
    }

    void visit(const ExpressionLast* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kLast);
    }

    void visit(const ExpressionObjectToArray* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kObjectToArray);
    }

    void visit(const ExpressionArrayToObject* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kArrayToObject);
    }

    void visit(const ExpressionBsonSize* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kArrayToObject);
    }

    void visit(const ExpressionCeil* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kCeil);
    }

    void visit(const ExpressionCoerceToBool* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kCoerceToBool);
    }

    void visit(const ExpressionCompare* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kCompare, expr->getOp());
    }

    void visit(const ExpressionConcat* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kConcat);
    }

    void visit(const ExpressionConcatArrays* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kConcatArrays);
    }

    void visit(const ExpressionCond* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kCond);
    }

    void visit(const ExpressionDateFromString* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kDateFromString);
        if (expr->_parsedTimeZone) {
            _hashState = H::combine(std::move(_hashState), expr->_parsedTimeZone);
        }
    }

    void visit(const ExpressionDateFromParts* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kDateFromParts);
        if (expr->_parsedTimeZone) {
            _hashState = H::combine(std::move(_hashState), expr->_parsedTimeZone);
        }
    }

    void visit(const ExpressionDateDiff* expr) final {
        _hashState = H::combine(
            std::move(_hashState), OpType::kDateDiff, expr->_parsedStartOfWeek, expr->_parsedUnit);
        if (expr->_parsedTimeZone) {
            _hashState = H::combine(std::move(_hashState), expr->_parsedTimeZone);
        }
    }

    void visit(const ExpressionDateToParts* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kDateToParts);
        if (expr->_parsedTimeZone) {
            _hashState = H::combine(std::move(_hashState), expr->_parsedTimeZone);
        }
    }

    void visit(const ExpressionDateToString* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kDateToString);
        if (expr->_parsedTimeZone) {
            _hashState = H::combine(std::move(_hashState), expr->_parsedTimeZone);
        }
    }

    void visit(const ExpressionDateTrunc* expr) final {
        _hashState = H::combine(std::move(_hashState),
                                OpType::kDateTrunc,
                                expr->_parsedStartOfWeek,
                                expr->_parsedUnit,
                                expr->_parsedBinSize);
        if (expr->_parsedTimeZone) {
            _hashState = H::combine(std::move(_hashState), expr->_parsedTimeZone);
        }
    }

    void visit(const ExpressionDivide* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kDivide);
    }

    void visit(const ExpressionExp* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kExp);
    }

    void visit(const ExpressionFieldPath* expr) final {
        _hashState = H::combine(
            std::move(_hashState), OpType::kExp, expr->getFieldPath(), expr->getVariableId());
    }

    void visit(const ExpressionFilter* expr) final {
        _hashState = H::combine(
            std::move(_hashState), OpType::kFilter, expr->_limit, expr->_varId, expr->_varName);
    }
    void visit(const ExpressionFloor* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kFloor);
    }

    void visit(const ExpressionIfNull* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kIfNull);
    }

    void visit(const ExpressionIn* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kIn);
    }

    void visit(const ExpressionIndexOfArray* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kIndexOfArray);
    }

    void visit(const ExpressionIndexOfBytes* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kIndexOfBytes);
    }

    void visit(const ExpressionIndexOfCP* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kIndexOfCP);
    }

    void visit(const ExpressionIsNumber* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kIsNumber);
    }

    void visit(const ExpressionLet* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kLet);
    }

    void visit(const ExpressionLn* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kLn);
    }

    void visit(const ExpressionLog* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kLog);
    }

    void visit(const ExpressionLog10* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kLog10);
    }

    void visit(const ExpressionInternalFLEBetween* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kInternalFLEBetween);
    }

    void visit(const ExpressionInternalFLEEqual* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kInternalFLEEqual);
    }

    void visit(const ExpressionMap* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kMap, expr->_varId, expr->_varName);
    }

    void visit(const ExpressionMeta* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kMeta, expr->getMetaType());
    }

    void visit(const ExpressionMod* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kMod);
    }

    void visit(const ExpressionMultiply* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kMultiply);
    }

    void visit(const ExpressionNot* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kNot);
    }

    void visit(const ExpressionObject* expr) final {
        // Expressions are not hashed here, only their keys, since the expressions are duplicated in
        // _children field and will be hashed by the walker.
        _hashState = H::combine(std::move(_hashState), OpType::kObject);
        for (const auto& childExpressionPair : expr->getChildExpressions()) {
            _hashState = H::combine(std::move(_hashState), childExpressionPair.first);
        }
    }

    void visit(const ExpressionOr* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kOr);
    }

    void visit(const ExpressionPow* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kPow);
    }

    void visit(const ExpressionRange* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kRange);
    }

    void visit(const ExpressionReduce* expr) final {
        _hashState =
            H::combine(std::move(_hashState), OpType::kReduce, expr->_thisVar, expr->_valueVar);
    }

    void visit(const ExpressionReplaceOne* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kReplaceOne);
    }

    void visit(const ExpressionReplaceAll* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kReplaceAll);
    }

    void visit(const ExpressionSetDifference* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kSetDifference);
    }

    void visit(const ExpressionSetEquals* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kSetEquals);
    }

    void visit(const ExpressionSetIntersection* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kSetIntersection);
    }

    void visit(const ExpressionSetIsSubset* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kSetIsSubset);
    }

    void visit(const ExpressionSetUnion* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kSetUnion);
    }

    void visit(const ExpressionSize* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kSize);
    }

    void visit(const ExpressionReverseArray* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kReverseArray);
    }

    void visit(const ExpressionSortArray* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kSortArray);
    }

    void visit(const ExpressionSlice* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kSlice);
    }

    void visit(const ExpressionIsArray* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kIsArray);
    }

    void visit(const ExpressionInternalFindAllValuesAtPath* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kInternalFindAllValuesAtPath);
    }

    void visit(const ExpressionRandom* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kRandom);
    }

    void visit(const ExpressionRound* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kRound);
    }

    void visit(const ExpressionSplit* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kSplit);
    }

    void visit(const ExpressionSqrt* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kSqrt);
    }

    void visit(const ExpressionStrcasecmp* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kStrcasecmp);
    }

    void visit(const ExpressionSubstrBytes* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kSubstrBytes);
    }

    void visit(const ExpressionSubstrCP* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kSubstrCP);
    }

    void visit(const ExpressionStrLenBytes* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kStrLenBytes);
    }

    void visit(const ExpressionBinarySize* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kBinarySize);
    }

    void visit(const ExpressionStrLenCP* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kStrLenCP);
    }

    void visit(const ExpressionSubtract* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kSubtract);
    }

    void visit(const ExpressionSwitch* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kSwitch);
    }

    void visit(const ExpressionToLower* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kToLower);
    }

    void visit(const ExpressionToUpper* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kToUpper);
    }

    void visit(const ExpressionTrim* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kTrim);
    }

    void visit(const ExpressionTrunc* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kTrunc);
    }

    void visit(const ExpressionType* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kType);
    }

    void visit(const ExpressionZip* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kZip);
    }

    void visit(const ExpressionConvert* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kConvert);
    }

    void visit(const ExpressionRegexFind* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kRegexFind);
    }

    void visit(const ExpressionRegexFindAll* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kRegexFindAll);
    }

    void visit(const ExpressionRegexMatch* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kRegexMatch);
    }

    void visit(const ExpressionCosine* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kCosine);
    }

    void visit(const ExpressionSine* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kSine);
    }

    void visit(const ExpressionTangent* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kTangent);
    }

    void visit(const ExpressionArcCosine* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kArcCosine);
    }

    void visit(const ExpressionArcSine* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kArcSine);
    }

    void visit(const ExpressionArcTangent* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kArcTangent);
    }

    void visit(const ExpressionArcTangent2* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kArcTangent2);
    }

    void visit(const ExpressionHyperbolicArcTangent* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kHyperbolicArcTangent);
    }

    void visit(const ExpressionHyperbolicArcCosine* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kHyperbolicArcCosine);
    }

    void visit(const ExpressionHyperbolicArcSine* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kHyperbolicArcSine);
    }

    void visit(const ExpressionHyperbolicTangent* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kHyperbolicTangent);
    }

    void visit(const ExpressionHyperbolicCosine* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kHyperbolicCosine);
    }

    void visit(const ExpressionHyperbolicSine* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kHyperbolicSine);
    }

    void visit(const ExpressionDegreesToRadians* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kDegreesToRadians);
    }

    void visit(const ExpressionRadiansToDegrees* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kRadiansToDegrees);
    }

    void visit(const ExpressionDayOfMonth* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kDayOfMonth);
    }

    void visit(const ExpressionDayOfWeek* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kDayOfWeek);
    }

    void visit(const ExpressionDayOfYear* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kDayOfYear);
    }

    void visit(const ExpressionHour* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kHour);
    }

    void visit(const ExpressionMillisecond* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kMillisecond);
    }

    void visit(const ExpressionMinute* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kMinute);
    }

    void visit(const ExpressionMonth* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kMonth);
    }

    void visit(const ExpressionSecond* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kSecond);
    }

    void visit(const ExpressionWeek* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kWeek);
    }

    void visit(const ExpressionIsoWeekYear* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kIsoWeekYear);
    }

    void visit(const ExpressionIsoDayOfWeek* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kIsoDayOfWeek);
    }

    void visit(const ExpressionIsoWeek* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kIsoWeek);
    }

    void visit(const ExpressionYear* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kYear);
    }

    void visit(const ExpressionFromAccumulator<AccumulatorAvg>* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kAccumulatorAvg);
    }

    void visit(const ExpressionFromAccumulatorN<AccumulatorFirstN>* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kAccumulatorFirstN);
    }

    void visit(const ExpressionFromAccumulatorN<AccumulatorLastN>* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kAccumulatorLastN);
    }

    void visit(const ExpressionFromAccumulator<AccumulatorMax>* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kAccumulatorMax);
    }

    void visit(const ExpressionFromAccumulator<AccumulatorMin>* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kAccumulatorMin);
    }

    void visit(const ExpressionFromAccumulatorN<AccumulatorMaxN>* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kAccumulatorMaxN);
    }

    void visit(const ExpressionFromAccumulatorN<AccumulatorMinN>* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kAccumulatorMinN);
    }

    void visit(const ExpressionFromAccumulatorQuantile<AccumulatorMedian>* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kAccumulatorMedian, expr->_method);
    }

    void visit(const ExpressionFromAccumulatorQuantile<AccumulatorPercentile>* expr) final {
        _hashState =
            H::combine(std::move(_hashState), OpType::kAccumulatorPercentile, expr->_method);
    }

    void visit(const ExpressionFromAccumulator<AccumulatorStdDevPop>* expr) {
        _hashState = H::combine(std::move(_hashState), OpType::kAccumulatorStdDevPop);
    }

    void visit(const ExpressionFromAccumulator<AccumulatorStdDevSamp>* expr) {
        _hashState = H::combine(std::move(_hashState), OpType::kAccumulatorStdDevSamp);
    }

    void visit(const ExpressionFromAccumulator<AccumulatorSum>* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kAccumulatorSum);
    }

    void visit(const ExpressionFromAccumulator<AccumulatorMergeObjects>* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kAccumulatorMergeObjects);
    }

    void visit(const ExpressionTests::Testable* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kTestable);
    }

    void visit(const ExpressionInternalJsEmit* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kInternalJsEmit);
    }

    void visit(const ExpressionFunction* expr) final {
        // _passedArgs are stored in _children field as well and will be hashed by the
        // expression_walker.
        _hashState = H::combine(std::move(_hashState),
                                OpType::kFunction,
                                expr->_assignFirstArgToThis,
                                expr->_funcSource,
                                expr->_lang);
    }

    void visit(const ExpressionInternalFindSlice* expr) final {
        _hashState = H::combine(
            std::move(_hashState), OpType::kInternalFindSlice, expr->_skip, expr->_limit);
    }

    void visit(const ExpressionInternalFindPositional* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kInternalFindPositional);
    }

    void visit(const ExpressionInternalFindElemMatch* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kInternalFindElemMatch);
    }

    void visit(const ExpressionToHashedIndexKey* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kToHashedIndexKey);
    }

    void visit(const ExpressionDateAdd* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kDateAdd);
    }

    void visit(const ExpressionDateSubtract* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kDateSubtract);
    }

    void visit(const ExpressionGetField* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kGetField);
    }

    void visit(const ExpressionSetField* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kSetField);
    }

    void visit(const ExpressionTsSecond* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kTsSecond);
    }

    void visit(const ExpressionTsIncrement* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kTsIncrement);
    }

    void visit(const ExpressionInternalOwningShard* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kInternalOwningShard);
    }

    void visit(const ExpressionInternalIndexKey* expr) final {
        _hashState = H::combine(std::move(_hashState), OpType::kInternalIndexKey);
    }

    H moveHashState() {
        return std::move(_hashState);
    }

private:
    H _hashState;
};

template <typename H>
H AbslHashValue(H h, const Expression& expr) {
    ExpressionHashVisitor visitor{std::move(h)};
    stage_builder::ExpressionWalker walker{&visitor, nullptr, nullptr};
    expression_walker::walk<const Expression>(&expr, &walker);
    return visitor.moveHashState();
}
}  // namespace mongo
