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
#include "mongo/db/pipeline/expression_from_accumulator_quantile.h"
#include "mongo/db/pipeline/expression_function.h"
#include "mongo/db/pipeline/expression_visitor.h"
#include "mongo/db/query/expression_walker.h"

namespace mongo {

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
        kEncStrStartsWith,
        kEncStrEndsWith,
        kEncStrContains,
        kEncStrNormalizedEq,
        kInternalRawSortKey,
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
        kDotProduct,
        kCosineSimilarity,
        kEuclideanDistance,
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
        kSubtype,
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
        kInternalKeyStringValue,
        kCurrentDate,
        kUUID,
        kOID,
        kTestFeatureFlagLatest,
        kTestFeatureFlagLastLTS
    };

    explicit ExpressionHashVisitor(H hashState) : _hashState(std::move(hashState)) {}

    void visit(const ExpressionConstant* expr) final {
        combine(OpType::kConst, expr->getValue());
    }

    void visit(const ExpressionAbs* expr) final {
        combine(OpType::kAbs);
    }

    void visit(const ExpressionAdd* expr) final {
        combine(OpType::kAdd);
    }

    void visit(const ExpressionAllElementsTrue* expr) final {
        combine(OpType::kAllElementsTrue);
    }

    void visit(const ExpressionAnd* expr) final {
        combine(OpType::kAnd);
    }

    void visit(const ExpressionAnyElementTrue* expr) final {
        combine(OpType::kAnyElementTrue);
    }

    void visit(const ExpressionTestApiVersion* expr) final {
        combine(OpType::kTestApiVersion);
    }

    void visit(const ExpressionArray* expr) final {
        combine(OpType::kArray);
    }

    void visit(const ExpressionArrayElemAt* expr) final {
        combine(OpType::kArrayElemAt);
    }

    void visit(const ExpressionBitAnd* expr) final {
        combine(OpType::kBitAnd);
    }

    void visit(const ExpressionBitOr* expr) final {
        combine(OpType::kBitOr);
    }

    void visit(const ExpressionBitXor* expr) final {
        combine(OpType::kBitXor);
    }

    void visit(const ExpressionBitNot* expr) final {
        combine(OpType::kBitNot);
    }

    void visit(const ExpressionFirst* expr) final {
        combine(OpType::kFirst);
    }

    void visit(const ExpressionLast* expr) final {
        combine(OpType::kLast);
    }

    void visit(const ExpressionObjectToArray* expr) final {
        combine(OpType::kObjectToArray);
    }

    void visit(const ExpressionArrayToObject* expr) final {
        combine(OpType::kArrayToObject);
    }

    void visit(const ExpressionBsonSize* expr) final {
        combine(OpType::kArrayToObject);
    }

    void visit(const ExpressionCeil* expr) final {
        combine(OpType::kCeil);
    }

    void visit(const ExpressionCompare* expr) final {
        combine(OpType::kCompare, expr->getOp());
    }

    void visit(const ExpressionConcat* expr) final {
        combine(OpType::kConcat);
    }

    void visit(const ExpressionConcatArrays* expr) final {
        combine(OpType::kConcatArrays);
    }

    void visit(const ExpressionCond* expr) final {
        combine(OpType::kCond);
    }

    void visit(const ExpressionDateFromString* expr) final {
        combine(OpType::kDateFromString);
        if (expr->_parsedTimeZone) {
            combine(expr->_parsedTimeZone);
        }
    }

    void visit(const ExpressionDateFromParts* expr) final {
        combine(OpType::kDateFromParts);
        if (expr->_parsedTimeZone) {
            combine(expr->_parsedTimeZone);
        }
    }

    void visit(const ExpressionDateDiff* expr) final {
        combine(OpType::kDateDiff, expr->_parsedStartOfWeek, expr->_parsedUnit);
        if (expr->_parsedTimeZone) {
            combine(expr->_parsedTimeZone);
        }
    }

    void visit(const ExpressionDateToParts* expr) final {
        combine(OpType::kDateToParts);
        if (expr->_parsedTimeZone) {
            combine(expr->_parsedTimeZone);
        }
    }

    void visit(const ExpressionDateToString* expr) final {
        combine(OpType::kDateToString);
        if (expr->_parsedTimeZone) {
            combine(expr->_parsedTimeZone);
        }
    }

    void visit(const ExpressionDateTrunc* expr) final {
        combine(
            OpType::kDateTrunc, expr->_parsedStartOfWeek, expr->_parsedUnit, expr->_parsedBinSize);
        if (expr->_parsedTimeZone) {
            combine(expr->_parsedTimeZone);
        }
    }

    void visit(const ExpressionDivide* expr) final {
        combine(OpType::kDivide);
    }

    void visit(const ExpressionExp* expr) final {
        combine(OpType::kExp);
    }

    void visit(const ExpressionFieldPath* expr) final {
        combine(OpType::kExp, expr->serialize());
    }

    void visit(const ExpressionFilter* expr) final {
        combine(OpType::kFilter, expr->_limit, expr->_varId, expr->_varName);
    }
    void visit(const ExpressionFloor* expr) final {
        combine(OpType::kFloor);
    }

    void visit(const ExpressionIfNull* expr) final {
        combine(OpType::kIfNull);
    }

    void visit(const ExpressionIn* expr) final {
        combine(OpType::kIn);
    }

    void visit(const ExpressionIndexOfArray* expr) final {
        combine(OpType::kIndexOfArray);
    }

    void visit(const ExpressionIndexOfBytes* expr) final {
        combine(OpType::kIndexOfBytes);
    }

    void visit(const ExpressionIndexOfCP* expr) final {
        combine(OpType::kIndexOfCP);
    }

    void visit(const ExpressionIsNumber* expr) final {
        combine(OpType::kIsNumber);
    }

    void visit(const ExpressionLet* expr) final {
        combine(OpType::kLet);
    }

    void visit(const ExpressionLn* expr) final {
        combine(OpType::kLn);
    }

    void visit(const ExpressionLog* expr) final {
        combine(OpType::kLog);
    }

    void visit(const ExpressionLog10* expr) final {
        combine(OpType::kLog10);
    }

    void visit(const ExpressionInternalFLEBetween* expr) final {
        combine(OpType::kInternalFLEBetween);
    }

    void visit(const ExpressionInternalFLEEqual* expr) final {
        combine(OpType::kInternalFLEEqual);
    }

    void visit(const ExpressionEncStrStartsWith* expr) final {
        combine(OpType::kEncStrStartsWith);
    }

    void visit(const ExpressionEncStrEndsWith* expr) final {
        combine(OpType::kEncStrEndsWith);
    }

    void visit(const ExpressionEncStrContains* expr) final {
        combine(OpType::kEncStrContains);
    }

    void visit(const ExpressionEncStrNormalizedEq* expr) final {
        combine(OpType::kEncStrNormalizedEq);
    }

    void visit(const ExpressionInternalRawSortKey* expr) final {
        combine(OpType::kInternalRawSortKey);
    }

    void visit(const ExpressionMap* expr) final {
        combine(OpType::kMap, expr->_varId, expr->_varName);
    }

    void visit(const ExpressionMeta* expr) final {
        combine(OpType::kMeta, expr->getMetaType());
    }

    void visit(const ExpressionMod* expr) final {
        combine(OpType::kMod);
    }

    void visit(const ExpressionMultiply* expr) final {
        combine(OpType::kMultiply);
    }

    void visit(const ExpressionNot* expr) final {
        combine(OpType::kNot);
    }

    void visit(const ExpressionObject* expr) final {
        // Expressions are not hashed here, only their keys, since the expressions are duplicated in
        // _children field and will be hashed by the walker.
        combine(OpType::kObject);
        for (const auto& childExpressionPair : expr->getChildExpressions()) {
            combine(childExpressionPair.first);
        }
    }

    void visit(const ExpressionOr* expr) final {
        combine(OpType::kOr);
    }

    void visit(const ExpressionPow* expr) final {
        combine(OpType::kPow);
    }

    void visit(const ExpressionRange* expr) final {
        combine(OpType::kRange);
    }

    void visit(const ExpressionReduce* expr) final {
        combine(OpType::kReduce, expr->_thisVar, expr->_valueVar);
    }

    void visit(const ExpressionReplaceOne* expr) final {
        combine(OpType::kReplaceOne);
    }

    void visit(const ExpressionReplaceAll* expr) final {
        combine(OpType::kReplaceAll);
    }

    void visit(const ExpressionSetDifference* expr) final {
        combine(OpType::kSetDifference);
    }

    void visit(const ExpressionSetEquals* expr) final {
        combine(OpType::kSetEquals);
    }

    void visit(const ExpressionSetIntersection* expr) final {
        combine(OpType::kSetIntersection);
    }

    void visit(const ExpressionSetIsSubset* expr) final {
        combine(OpType::kSetIsSubset);
    }

    void visit(const ExpressionSetUnion* expr) final {
        combine(OpType::kSetUnion);
    }

    void visit(const ExpressionSimilarityDotProduct* expr) final {
        combine(OpType::kDotProduct);
    }

    void visit(const ExpressionSimilarityCosine* expr) final {
        combine(OpType::kCosineSimilarity);
    }

    void visit(const ExpressionSimilarityEuclidean* expr) final {
        combine(OpType::kEuclideanDistance);
    }

    void visit(const ExpressionSize* expr) final {
        combine(OpType::kSize);
    }

    void visit(const ExpressionReverseArray* expr) final {
        combine(OpType::kReverseArray);
    }

    void visit(const ExpressionSortArray* expr) final {
        combine(OpType::kSortArray);
    }

    void visit(const ExpressionSlice* expr) final {
        combine(OpType::kSlice);
    }

    void visit(const ExpressionIsArray* expr) final {
        combine(OpType::kIsArray);
    }

    void visit(const ExpressionInternalFindAllValuesAtPath* expr) final {
        combine(OpType::kInternalFindAllValuesAtPath);
    }

    void visit(const ExpressionRandom* expr) final {
        combine(OpType::kRandom);
    }

    void visit(const ExpressionCurrentDate* expr) final {
        combine(OpType::kCurrentDate);
    }

    void visit(const ExpressionRound* expr) final {
        combine(OpType::kRound);
    }

    void visit(const ExpressionSplit* expr) final {
        combine(OpType::kSplit);
    }

    void visit(const ExpressionSqrt* expr) final {
        combine(OpType::kSqrt);
    }

    void visit(const ExpressionStrcasecmp* expr) final {
        combine(OpType::kStrcasecmp);
    }

    void visit(const ExpressionSubstrBytes* expr) final {
        combine(OpType::kSubstrBytes);
    }

    void visit(const ExpressionSubstrCP* expr) final {
        combine(OpType::kSubstrCP);
    }

    void visit(const ExpressionStrLenBytes* expr) final {
        combine(OpType::kStrLenBytes);
    }

    void visit(const ExpressionBinarySize* expr) final {
        combine(OpType::kBinarySize);
    }

    void visit(const ExpressionStrLenCP* expr) final {
        combine(OpType::kStrLenCP);
    }

    void visit(const ExpressionSubtract* expr) final {
        combine(OpType::kSubtract);
    }

    void visit(const ExpressionSwitch* expr) final {
        combine(OpType::kSwitch);
    }

    void visit(const ExpressionToLower* expr) final {
        combine(OpType::kToLower);
    }

    void visit(const ExpressionToUpper* expr) final {
        combine(OpType::kToUpper);
    }

    void visit(const ExpressionTrim* expr) final {
        combine(OpType::kTrim);
    }

    void visit(const ExpressionTrunc* expr) final {
        combine(OpType::kTrunc);
    }

    void visit(const ExpressionType* expr) final {
        combine(OpType::kType);
    }

    void visit(const ExpressionSubtype* expr) final {
        combine(OpType::kSubtype);
    }

    void visit(const ExpressionZip* expr) final {
        combine(OpType::kZip);
    }

    void visit(const ExpressionConvert* expr) final {
        combine(OpType::kConvert);
    }

    void visit(const ExpressionRegexFind* expr) final {
        combine(OpType::kRegexFind);
    }

    void visit(const ExpressionRegexFindAll* expr) final {
        combine(OpType::kRegexFindAll);
    }

    void visit(const ExpressionRegexMatch* expr) final {
        combine(OpType::kRegexMatch);
    }

    void visit(const ExpressionCosine* expr) final {
        combine(OpType::kCosine);
    }

    void visit(const ExpressionSine* expr) final {
        combine(OpType::kSine);
    }

    void visit(const ExpressionTangent* expr) final {
        combine(OpType::kTangent);
    }

    void visit(const ExpressionArcCosine* expr) final {
        combine(OpType::kArcCosine);
    }

    void visit(const ExpressionArcSine* expr) final {
        combine(OpType::kArcSine);
    }

    void visit(const ExpressionArcTangent* expr) final {
        combine(OpType::kArcTangent);
    }

    void visit(const ExpressionArcTangent2* expr) final {
        combine(OpType::kArcTangent2);
    }

    void visit(const ExpressionHyperbolicArcTangent* expr) final {
        combine(OpType::kHyperbolicArcTangent);
    }

    void visit(const ExpressionHyperbolicArcCosine* expr) final {
        combine(OpType::kHyperbolicArcCosine);
    }

    void visit(const ExpressionHyperbolicArcSine* expr) final {
        combine(OpType::kHyperbolicArcSine);
    }

    void visit(const ExpressionHyperbolicTangent* expr) final {
        combine(OpType::kHyperbolicTangent);
    }

    void visit(const ExpressionHyperbolicCosine* expr) final {
        combine(OpType::kHyperbolicCosine);
    }

    void visit(const ExpressionHyperbolicSine* expr) final {
        combine(OpType::kHyperbolicSine);
    }

    void visit(const ExpressionDegreesToRadians* expr) final {
        combine(OpType::kDegreesToRadians);
    }

    void visit(const ExpressionRadiansToDegrees* expr) final {
        combine(OpType::kRadiansToDegrees);
    }

    void visit(const ExpressionDayOfMonth* expr) final {
        combine(OpType::kDayOfMonth);
    }

    void visit(const ExpressionDayOfWeek* expr) final {
        combine(OpType::kDayOfWeek);
    }

    void visit(const ExpressionDayOfYear* expr) final {
        combine(OpType::kDayOfYear);
    }

    void visit(const ExpressionHour* expr) final {
        combine(OpType::kHour);
    }

    void visit(const ExpressionMillisecond* expr) final {
        combine(OpType::kMillisecond);
    }

    void visit(const ExpressionMinute* expr) final {
        combine(OpType::kMinute);
    }

    void visit(const ExpressionMonth* expr) final {
        combine(OpType::kMonth);
    }

    void visit(const ExpressionSecond* expr) final {
        combine(OpType::kSecond);
    }

    void visit(const ExpressionWeek* expr) final {
        // constexpr static std::unordered_map<std::type_index, OpType> typeTo
        combine(OpType::kWeek);
    }

    void visit(const ExpressionIsoWeekYear* expr) final {
        combine(OpType::kIsoWeekYear);
    }

    void visit(const ExpressionIsoDayOfWeek* expr) final {
        combine(OpType::kIsoDayOfWeek);
    }

    void visit(const ExpressionIsoWeek* expr) final {
        combine(OpType::kIsoWeek);
    }

    void visit(const ExpressionYear* expr) final {
        combine(OpType::kYear);
    }

    void visit(const ExpressionFromAccumulator<AccumulatorAvg>* expr) final {
        combine(OpType::kAccumulatorAvg);
    }

    void visit(const ExpressionFromAccumulatorN<AccumulatorFirstN>* expr) final {
        combine(OpType::kAccumulatorFirstN);
    }

    void visit(const ExpressionFromAccumulatorN<AccumulatorLastN>* expr) final {
        combine(OpType::kAccumulatorLastN);
    }

    void visit(const ExpressionFromAccumulator<AccumulatorMax>* expr) final {
        combine(OpType::kAccumulatorMax);
    }

    void visit(const ExpressionFromAccumulator<AccumulatorMin>* expr) final {
        combine(OpType::kAccumulatorMin);
    }

    void visit(const ExpressionFromAccumulatorN<AccumulatorMaxN>* expr) final {
        combine(OpType::kAccumulatorMaxN);
    }

    void visit(const ExpressionFromAccumulatorN<AccumulatorMinN>* expr) final {
        combine(OpType::kAccumulatorMinN);
    }

    void visit(const ExpressionFromAccumulatorQuantile<AccumulatorMedian>* expr) final {
        combine(OpType::kAccumulatorMedian, expr->_method);
    }

    void visit(const ExpressionFromAccumulatorQuantile<AccumulatorPercentile>* expr) final {
        combine(OpType::kAccumulatorPercentile, expr->_method);
    }

    void visit(const ExpressionFromAccumulator<AccumulatorStdDevPop>* expr) override {
        combine(OpType::kAccumulatorStdDevPop);
    }

    void visit(const ExpressionFromAccumulator<AccumulatorStdDevSamp>* expr) override {
        combine(OpType::kAccumulatorStdDevSamp);
    }

    void visit(const ExpressionFromAccumulator<AccumulatorSum>* expr) final {
        combine(OpType::kAccumulatorSum);
    }

    void visit(const ExpressionFromAccumulator<AccumulatorMergeObjects>* expr) final {
        combine(OpType::kAccumulatorMergeObjects);
    }

    void visit(const ExpressionTests::Testable* expr) final {
        combine(OpType::kTestable);
    }

    void visit(const ExpressionInternalJsEmit* expr) final {
        combine(OpType::kInternalJsEmit);
    }

    void visit(const ExpressionFunction* expr) final {
        // _passedArgs are stored in _children field as well and will be hashed by the
        // expression_walker.
        combine(OpType::kFunction, expr->_assignFirstArgToThis, expr->_funcSource, expr->_lang);
    }

    void visit(const ExpressionInternalFindSlice* expr) final {
        combine(OpType::kInternalFindSlice, expr->_skip, expr->_limit);
    }

    void visit(const ExpressionInternalFindPositional* expr) final {
        combine(OpType::kInternalFindPositional);
    }

    void visit(const ExpressionInternalFindElemMatch* expr) final {
        combine(OpType::kInternalFindElemMatch);
    }

    void visit(const ExpressionToHashedIndexKey* expr) final {
        combine(OpType::kToHashedIndexKey);
    }

    void visit(const ExpressionDateAdd* expr) final {
        combine(OpType::kDateAdd);
    }

    void visit(const ExpressionDateSubtract* expr) final {
        combine(OpType::kDateSubtract);
    }

    void visit(const ExpressionGetField* expr) final {
        combine(OpType::kGetField);
    }

    void visit(const ExpressionSetField* expr) final {
        combine(OpType::kSetField);
    }

    void visit(const ExpressionTsSecond* expr) final {
        combine(OpType::kTsSecond);
    }

    void visit(const ExpressionTsIncrement* expr) final {
        combine(OpType::kTsIncrement);
    }

    void visit(const ExpressionInternalOwningShard* expr) final {
        combine(OpType::kInternalOwningShard);
    }

    void visit(const ExpressionInternalIndexKey* expr) final {
        combine(OpType::kInternalIndexKey);
    }

    void visit(const ExpressionInternalKeyStringValue* expr) final {
        combine(OpType::kInternalKeyStringValue);
    }

    void visit(const ExpressionCreateUUID* expr) final {
        combine(OpType::kUUID);
    }

    void visit(const ExpressionCreateObjectId* expr) final {
        combine(OpType::kOID);
    }

    void visit(const ExpressionTestFeatureFlagLatest* expr) final {
        combine(OpType::kTestFeatureFlagLatest);
    }

    void visit(const ExpressionTestFeatureFlagLastLTS* expr) final {
        combine(OpType::kTestFeatureFlagLastLTS);
    }

    H moveHashState() {
        return std::move(_hashState);
    }

private:
    template <typename T>
    class PrivateHash {
    private:
        /** Fallback */
        template <typename TT>
        static H _doHash(H h, const TT& x) {
            return H::combine(std::move(h), x);
        }

        static H _doHash(H h, const Value& value) {
            return H::combine(std::move(h), ValueComparator{}.hash(value));
        }

        static H _doHash(H h, const TimeZone& tz) {
            return H::combine(std::move(h), tz.toString());
        }

        template <typename TT>
        static H _doHash(H h, const boost::optional<TT>& x) {
            if (x)
                h = _doHash(std::move(h), *x);
            h = _doHash(std::move(h), bool{x});
            return h;
        }

        template <typename HH>
        friend HH AbslHashValue(HH h, const PrivateHash& hashable) {
            return _doHash(std::move(h), hashable.value);
        }

    public:
        const T& value;
    };

    template <typename... Ts>
    void combine(const Ts&... values) {
        _hashState = H::combine(std::move(_hashState), PrivateHash<Ts>{values}...);
    }

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
