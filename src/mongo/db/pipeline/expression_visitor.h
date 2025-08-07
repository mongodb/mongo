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

#include "mongo/db/pipeline/expression_walker.h"

namespace ExpressionTests {
class Testable;
}  // namespace ExpressionTests

namespace mongo {

class ExpressionConstant;
class ExpressionAbs;
class ExpressionAdd;
class ExpressionAllElementsTrue;
class ExpressionAnd;
class ExpressionAnyElementTrue;
class ExpressionArray;
class ExpressionArrayElemAt;
class ExpressionBitNot;
class ExpressionFirst;
class ExpressionLast;
class ExpressionObjectToArray;
class ExpressionArrayToObject;
class ExpressionBsonSize;
class ExpressionCeil;
class ExpressionCompare;
class ExpressionConcat;
class ExpressionConcatArrays;
class ExpressionCond;
class ExpressionDateFromString;
class ExpressionDateFromParts;
class ExpressionDateToParts;
class ExpressionDateToString;
class ExpressionDayOfMonth;
class ExpressionDayOfWeek;
class ExpressionDayOfYear;
class ExpressionDivide;
class ExpressionExp;
class ExpressionFieldPath;
class ExpressionFilter;
class ExpressionFloor;
class ExpressionToHashedIndexKey;
class ExpressionHour;
class ExpressionIfNull;
class ExpressionIn;
class ExpressionIndexOfArray;
class ExpressionIndexOfBytes;
class ExpressionIndexOfCP;
class ExpressionIsNumber;
class ExpressionLet;
class ExpressionLn;
class ExpressionLog;
class ExpressionLog10;
class ExpressionMap;
class ExpressionMeta;
class ExpressionMillisecond;
class ExpressionMinute;
class ExpressionMod;
class ExpressionMonth;
class ExpressionMultiply;
class ExpressionNot;
class ExpressionObject;
class ExpressionOr;
class ExpressionPow;
class ExpressionRange;
class ExpressionReduce;
class ExpressionReplaceOne;
class ExpressionReplaceAll;
class ExpressionSetDifference;
class ExpressionSetEquals;
class ExpressionSetIntersection;
class ExpressionSetIsSubset;
class ExpressionSetUnion;
class ExpressionSimilarityDotProduct;
class ExpressionSimilarityCosine;
class ExpressionSimilarityEuclidean;
class ExpressionSize;
class ExpressionReverseArray;
class ExpressionSortArray;
class ExpressionSlice;
class ExpressionIsArray;
class ExpressionInternalFindAllValuesAtPath;
class ExpressionRandom;
class ExpressionCurrentDate;
class ExpressionRound;
class ExpressionSecond;
class ExpressionSplit;
class ExpressionSqrt;
class ExpressionStrcasecmp;
class ExpressionSubstrBytes;
class ExpressionSubstrCP;
class ExpressionStrLenBytes;
class ExpressionBinarySize;
class ExpressionStrLenCP;
class ExpressionSubtract;
class ExpressionSubtype;
class ExpressionSwitch;
class ExpressionTestApiVersion;
class ExpressionToLower;
class ExpressionToUpper;
class ExpressionTrim;
class ExpressionTrunc;
class ExpressionType;
class ExpressionZip;
class ExpressionConvert;
class ExpressionRegexFind;
class ExpressionRegexFindAll;
class ExpressionRegexMatch;
class ExpressionWeek;
class ExpressionIsoWeekYear;
class ExpressionIsoDayOfWeek;
class ExpressionIsoWeek;
class ExpressionYear;
class ExpressionCosine;
class ExpressionSine;
class ExpressionTangent;
class ExpressionArcCosine;
class ExpressionArcSine;
class ExpressionArcTangent;
class ExpressionArcTangent2;
class ExpressionHyperbolicArcTangent;
class ExpressionHyperbolicArcCosine;
class ExpressionHyperbolicArcSine;
class ExpressionHyperbolicTangent;
class ExpressionHyperbolicCosine;
class ExpressionHyperbolicSine;
class ExpressionInternalFindSlice;
class ExpressionInternalFindPositional;
class ExpressionInternalFindElemMatch;
class ExpressionInternalFLEBetween;
class ExpressionInternalFLEEqual;
class ExpressionInternalRawSortKey;
class ExpressionInternalIndexKey;
class ExpressionInternalJsEmit;
class ExpressionInternalOwningShard;
class ExpressionFunction;
class ExpressionDegreesToRadians;
class ExpressionRadiansToDegrees;
class ExpressionDateDiff;
class ExpressionDateAdd;
class ExpressionDateSubtract;
class ExpressionDateTrunc;
class ExpressionGetField;
class ExpressionSetField;
class ExpressionBitAnd;
class ExpressionBitOr;
class ExpressionBitXor;
class ExpressionInternalKeyStringValue;
class ExpressionCreateUUID;
class ExpressionEncStrStartsWith;
class ExpressionEncStrEndsWith;
class ExpressionEncStrContains;
class ExpressionEncStrNormalizedEq;
class ExpressionCreateObjectId;
class ExpressionTestFeatureFlagLatest;
class ExpressionTestFeatureFlagLastLTS;

class AccumulatorAvg;
class AccumulatorFirstN;
class AccumulatorLastN;
class AccumulatorMax;
class AccumulatorMin;
class AccumulatorMaxN;
class AccumulatorMinN;
class AccumulatorMedian;
class AccumulatorPercentile;
class AccumulatorStdDevPop;
class AccumulatorStdDevSamp;
class AccumulatorSum;
class AccumulatorMergeObjects;

class ExpressionTsSecond;
class ExpressionTsIncrement;

template <typename AccumulatorState>
class ExpressionFromAccumulator;
template <typename AccumulatorN>
class ExpressionFromAccumulatorN;

template <typename TAccumulator>
class ExpressionFromAccumulatorQuantile;

/**
 * This is a base class to allow for traversal of an aggregation expression tree. It implements the
 * visitor pattern, in which every derived class from Expression implements an accept() method,
 * which simply calls the appropriate visit() method on the derived ExpressionVisitor class. The
 * derived class can do whatever it needs to do for each specific node type in the corresponding
 * visit() method.
 *
 * Derived classes are responsible for making the recursive calls to visit() if they wish
 * to visit all the nodes in the expression tree. ExpressionVisitor's purpose is not actually to
 * ensure that every node in the tree is visited, but rather to handle dynamic dispatch without
 * having to add virtual methods to the Expression interface itself.
 *
 * If the visitor doesn't intend to modify the tree, then the template argument 'IsConst' should be
 * set to 'true'. In this case all 'visit()' methods will take a const pointer to a visiting node.
 */

template <bool IsConst>
class ExpressionVisitor {
public:
    virtual ~ExpressionVisitor() = default;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionConstant>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionAbs>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionAdd>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionAllElementsTrue>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionAnd>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionAnyElementTrue>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionTestApiVersion>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionArray>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionArrayElemAt>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionBitAnd>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionBitOr>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionBitXor>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionBitNot>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionFirst>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionLast>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionObjectToArray>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionArrayToObject>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionBsonSize>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionCeil>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionCompare>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionConcat>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionConcatArrays>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionCond>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionDateFromString>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionDateFromParts>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionDateDiff>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionDateToParts>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionDateToString>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionDateTrunc>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionDivide>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionExp>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionFieldPath>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionFilter>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionFloor>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionIfNull>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionIn>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionIndexOfArray>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionIndexOfBytes>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionIndexOfCP>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionIsNumber>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionLet>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionLn>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionLog>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionLog10>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionInternalFLEBetween>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionInternalFLEEqual>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionEncStrStartsWith>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionEncStrEndsWith>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionEncStrContains>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionEncStrNormalizedEq>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionInternalRawSortKey>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionMap>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionMeta>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionMod>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionMultiply>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionNot>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionObject>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionOr>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionPow>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionRange>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionReduce>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionReplaceOne>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionReplaceAll>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionSetDifference>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionSetEquals>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionSetIntersection>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionSetIsSubset>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionSetUnion>) = 0;
    virtual void visit(
        expression_walker::MaybeConstPtr<IsConst, ExpressionSimilarityDotProduct>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionSimilarityCosine>) = 0;
    virtual void visit(
        expression_walker::MaybeConstPtr<IsConst, ExpressionSimilarityEuclidean>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionSize>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionReverseArray>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionSortArray>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionSlice>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionIsArray>) = 0;
    virtual void visit(
        expression_walker::MaybeConstPtr<IsConst, ExpressionInternalFindAllValuesAtPath>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionRandom>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionCurrentDate>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionRound>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionSplit>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionSqrt>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionStrcasecmp>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionSubstrBytes>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionSubstrCP>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionStrLenBytes>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionBinarySize>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionStrLenCP>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionSubtract>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionSubtype>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionSwitch>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionToLower>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionToUpper>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionTrim>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionTrunc>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionType>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionZip>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionConvert>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionRegexFind>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionRegexFindAll>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionRegexMatch>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionCosine>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionSine>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionTangent>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionArcCosine>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionArcSine>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionArcTangent>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionArcTangent2>) = 0;
    virtual void visit(
        expression_walker::MaybeConstPtr<IsConst, ExpressionHyperbolicArcTangent>) = 0;
    virtual void visit(
        expression_walker::MaybeConstPtr<IsConst, ExpressionHyperbolicArcCosine>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionHyperbolicArcSine>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionHyperbolicTangent>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionHyperbolicCosine>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionHyperbolicSine>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionDegreesToRadians>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionRadiansToDegrees>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionDayOfMonth>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionDayOfWeek>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionDayOfYear>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionHour>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionMillisecond>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionMinute>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionMonth>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionSecond>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionWeek>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionIsoWeekYear>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionIsoDayOfWeek>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionIsoWeek>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionYear>) = 0;
    virtual void visit(
        expression_walker::MaybeConstPtr<IsConst, ExpressionFromAccumulator<AccumulatorAvg>>) = 0;
    virtual void visit(
        expression_walker::MaybeConstPtr<IsConst,
                                         ExpressionFromAccumulatorN<AccumulatorFirstN>>) = 0;
    virtual void visit(
        expression_walker::MaybeConstPtr<IsConst,
                                         ExpressionFromAccumulatorN<AccumulatorLastN>>) = 0;
    virtual void visit(
        expression_walker::MaybeConstPtr<IsConst, ExpressionFromAccumulator<AccumulatorMax>>) = 0;
    virtual void visit(
        expression_walker::MaybeConstPtr<IsConst, ExpressionFromAccumulator<AccumulatorMin>>) = 0;
    virtual void visit(
        expression_walker::MaybeConstPtr<IsConst, ExpressionFromAccumulatorN<AccumulatorMaxN>>) = 0;
    virtual void visit(
        expression_walker::MaybeConstPtr<IsConst, ExpressionFromAccumulatorN<AccumulatorMinN>>) = 0;
    virtual void visit(
        expression_walker::MaybeConstPtr<IsConst,
                                         ExpressionFromAccumulatorQuantile<AccumulatorMedian>>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<
                       IsConst,
                       ExpressionFromAccumulatorQuantile<AccumulatorPercentile>>) = 0;
    virtual void visit(
        expression_walker::MaybeConstPtr<IsConst,
                                         ExpressionFromAccumulator<AccumulatorStdDevPop>>) = 0;
    virtual void visit(
        expression_walker::MaybeConstPtr<IsConst,
                                         ExpressionFromAccumulator<AccumulatorStdDevSamp>>) = 0;
    virtual void visit(
        expression_walker::MaybeConstPtr<IsConst, ExpressionFromAccumulator<AccumulatorSum>>) = 0;
    virtual void visit(
        expression_walker::MaybeConstPtr<IsConst,
                                         ExpressionFromAccumulator<AccumulatorMergeObjects>>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionTests::Testable>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionInternalJsEmit>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionFunction>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionInternalFindSlice>) = 0;
    virtual void visit(
        expression_walker::MaybeConstPtr<IsConst, ExpressionInternalFindPositional>) = 0;
    virtual void visit(
        expression_walker::MaybeConstPtr<IsConst, ExpressionInternalFindElemMatch>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionToHashedIndexKey>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionDateAdd>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionDateSubtract>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionGetField>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionSetField>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionTsSecond>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionTsIncrement>) = 0;
    virtual void visit(
        expression_walker::MaybeConstPtr<IsConst, ExpressionInternalOwningShard>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionInternalIndexKey>) = 0;
    virtual void visit(
        expression_walker::MaybeConstPtr<IsConst, ExpressionInternalKeyStringValue>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionCreateUUID>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionCreateObjectId>) = 0;
    virtual void visit(
        expression_walker::MaybeConstPtr<IsConst, ExpressionTestFeatureFlagLatest>) = 0;
    virtual void visit(
        expression_walker::MaybeConstPtr<IsConst, ExpressionTestFeatureFlagLastLTS>) = 0;
};

using ExpressionMutableVisitor = ExpressionVisitor<false>;
using ExpressionConstVisitor = ExpressionVisitor<true>;

/**
 * This class provides null implementations for all visit methods so that a derived class can
 * override visit method(s) only for interested 'Expression' types. For example, if one wants
 * to visit only 'ExpressionFieldPath', one can override only void visit(const
 * ExpressionFieldPath*).
 *
 * struct FieldPathVisitor : public SelectiveConstExpressionVisitorBase {
 *     // To avoid overloaded-virtual warnings.
 *     using SelectiveConstExpressionVisitorBase::visit;
 *
 *     void visit(const ExpressionFieldPath* expr) final {
 *         // logic for what to do with an ExpressionFieldPath.
 *     }
 * };
 */
struct SelectiveConstExpressionVisitorBase : public ExpressionConstVisitor {
    void visit(const ExpressionConstant*) override {}
    void visit(const ExpressionAbs*) override {}
    void visit(const ExpressionAdd*) override {}
    void visit(const ExpressionAllElementsTrue*) override {}
    void visit(const ExpressionAnd*) override {}
    void visit(const ExpressionAnyElementTrue*) override {}
    void visit(const ExpressionArray*) override {}
    void visit(const ExpressionArrayElemAt*) override {}
    void visit(const ExpressionBitAnd*) override {}
    void visit(const ExpressionBitOr*) override {}
    void visit(const ExpressionBitXor*) override {}
    void visit(const ExpressionBitNot*) override {}
    void visit(const ExpressionFirst*) override {}
    void visit(const ExpressionLast*) override {}
    void visit(const ExpressionObjectToArray*) override {}
    void visit(const ExpressionArrayToObject*) override {}
    void visit(const ExpressionBsonSize*) override {}
    void visit(const ExpressionCeil*) override {}
    void visit(const ExpressionCompare*) override {}
    void visit(const ExpressionConcat*) override {}
    void visit(const ExpressionConcatArrays*) override {}
    void visit(const ExpressionCond*) override {}
    void visit(const ExpressionDateDiff*) override {}
    void visit(const ExpressionDateFromString*) override {}
    void visit(const ExpressionDateFromParts*) override {}
    void visit(const ExpressionDateToParts*) override {}
    void visit(const ExpressionDateToString*) override {}
    void visit(const ExpressionDateTrunc*) override {}
    void visit(const ExpressionDivide*) override {}
    void visit(const ExpressionExp*) override {}
    void visit(const ExpressionFieldPath*) override {}
    void visit(const ExpressionFilter*) override {}
    void visit(const ExpressionFloor*) override {}
    void visit(const ExpressionIfNull*) override {}
    void visit(const ExpressionIn*) override {}
    void visit(const ExpressionIndexOfArray*) override {}
    void visit(const ExpressionIndexOfBytes*) override {}
    void visit(const ExpressionIndexOfCP*) override {}
    void visit(const ExpressionIsNumber*) override {}
    void visit(const ExpressionLet*) override {}
    void visit(const ExpressionLn*) override {}
    void visit(const ExpressionLog*) override {}
    void visit(const ExpressionLog10*) override {}
    void visit(const ExpressionInternalFLEBetween*) override {}
    void visit(const ExpressionInternalFLEEqual*) override {}
    void visit(const ExpressionEncStrStartsWith*) override {}
    void visit(const ExpressionEncStrEndsWith*) override {}
    void visit(const ExpressionEncStrContains*) override {}
    void visit(const ExpressionEncStrNormalizedEq*) override {}
    void visit(const ExpressionInternalRawSortKey*) override {}
    void visit(const ExpressionMap*) override {}
    void visit(const ExpressionMeta*) override {}
    void visit(const ExpressionMod*) override {}
    void visit(const ExpressionMultiply*) override {}
    void visit(const ExpressionNot*) override {}
    void visit(const ExpressionObject*) override {}
    void visit(const ExpressionOr*) override {}
    void visit(const ExpressionPow*) override {}
    void visit(const ExpressionRange*) override {}
    void visit(const ExpressionReduce*) override {}
    void visit(const ExpressionReplaceOne*) override {}
    void visit(const ExpressionReplaceAll*) override {}
    void visit(const ExpressionSetDifference*) override {}
    void visit(const ExpressionSetEquals*) override {}
    void visit(const ExpressionSetIntersection*) override {}
    void visit(const ExpressionSetIsSubset*) override {}
    void visit(const ExpressionSetUnion*) override {}
    void visit(const ExpressionSimilarityDotProduct*) override {}
    void visit(const ExpressionSimilarityCosine*) override {}
    void visit(const ExpressionSimilarityEuclidean*) override {}
    void visit(const ExpressionSize*) override {}
    void visit(const ExpressionReverseArray*) override {}
    void visit(const ExpressionSortArray*) override {}
    void visit(const ExpressionSlice*) override {}
    void visit(const ExpressionIsArray*) override {}
    void visit(const ExpressionInternalFindAllValuesAtPath*) override {}
    void visit(const ExpressionRound*) override {}
    void visit(const ExpressionSplit*) override {}
    void visit(const ExpressionSqrt*) override {}
    void visit(const ExpressionStrcasecmp*) override {}
    void visit(const ExpressionSubstrBytes*) override {}
    void visit(const ExpressionSubstrCP*) override {}
    void visit(const ExpressionStrLenBytes*) override {}
    void visit(const ExpressionBinarySize*) override {}
    void visit(const ExpressionStrLenCP*) override {}
    void visit(const ExpressionSubtract*) override {}
    void visit(const ExpressionSubtype*) override {}
    void visit(const ExpressionSwitch*) override {}
    void visit(const ExpressionTestApiVersion*) override {}
    void visit(const ExpressionToLower*) override {}
    void visit(const ExpressionToUpper*) override {}
    void visit(const ExpressionTrim*) override {}
    void visit(const ExpressionTrunc*) override {}
    void visit(const ExpressionType*) override {}
    void visit(const ExpressionZip*) override {}
    void visit(const ExpressionConvert*) override {}
    void visit(const ExpressionRegexFind*) override {}
    void visit(const ExpressionRegexFindAll*) override {}
    void visit(const ExpressionRegexMatch*) override {}
    void visit(const ExpressionCosine*) override {}
    void visit(const ExpressionSine*) override {}
    void visit(const ExpressionTangent*) override {}
    void visit(const ExpressionArcCosine*) override {}
    void visit(const ExpressionArcSine*) override {}
    void visit(const ExpressionArcTangent*) override {}
    void visit(const ExpressionArcTangent2*) override {}
    void visit(const ExpressionHyperbolicArcTangent*) override {}
    void visit(const ExpressionHyperbolicArcCosine*) override {}
    void visit(const ExpressionHyperbolicArcSine*) override {}
    void visit(const ExpressionHyperbolicTangent*) override {}
    void visit(const ExpressionHyperbolicCosine*) override {}
    void visit(const ExpressionHyperbolicSine*) override {}
    void visit(const ExpressionDegreesToRadians*) override {}
    void visit(const ExpressionRadiansToDegrees*) override {}
    void visit(const ExpressionDayOfMonth*) override {}
    void visit(const ExpressionDayOfWeek*) override {}
    void visit(const ExpressionDayOfYear*) override {}
    void visit(const ExpressionHour*) override {}
    void visit(const ExpressionMillisecond*) override {}
    void visit(const ExpressionMinute*) override {}
    void visit(const ExpressionMonth*) override {}
    void visit(const ExpressionSecond*) override {}
    void visit(const ExpressionWeek*) override {}
    void visit(const ExpressionIsoWeekYear*) override {}
    void visit(const ExpressionIsoDayOfWeek*) override {}
    void visit(const ExpressionIsoWeek*) override {}
    void visit(const ExpressionYear*) override {}
    void visit(const ExpressionFromAccumulator<AccumulatorAvg>*) override {}
    void visit(const ExpressionFromAccumulator<AccumulatorMax>*) override {}
    void visit(const ExpressionFromAccumulator<AccumulatorMin>*) override {}
    void visit(const ExpressionFromAccumulatorN<AccumulatorFirstN>*) override {}
    void visit(const ExpressionFromAccumulatorN<AccumulatorLastN>*) override {}
    void visit(const ExpressionFromAccumulatorN<AccumulatorMaxN>*) override {}
    void visit(const ExpressionFromAccumulatorN<AccumulatorMinN>*) override {}
    void visit(const ExpressionFromAccumulatorQuantile<AccumulatorMedian>*) override {}
    void visit(const ExpressionFromAccumulatorQuantile<AccumulatorPercentile>*) override {}
    void visit(const ExpressionFromAccumulator<AccumulatorStdDevPop>*) override {}
    void visit(const ExpressionFromAccumulator<AccumulatorStdDevSamp>*) override {}
    void visit(const ExpressionFromAccumulator<AccumulatorSum>*) override {}
    void visit(const ExpressionFromAccumulator<AccumulatorMergeObjects>*) override {}
    void visit(const ExpressionTests::Testable*) override {}
    void visit(const ExpressionInternalJsEmit*) override {}
    void visit(const ExpressionInternalFindSlice*) override {}
    void visit(const ExpressionInternalFindPositional*) override {}
    void visit(const ExpressionInternalFindElemMatch*) override {}
    void visit(const ExpressionFunction*) override {}
    void visit(const ExpressionRandom*) override {}
    void visit(const ExpressionCurrentDate*) override {}
    void visit(const ExpressionToHashedIndexKey*) override {}
    void visit(const ExpressionDateAdd*) override {}
    void visit(const ExpressionDateSubtract*) override {}
    void visit(const ExpressionGetField*) override {}
    void visit(const ExpressionSetField*) override {}
    void visit(const ExpressionTsSecond*) override {}
    void visit(const ExpressionTsIncrement*) override {}
    void visit(const ExpressionInternalOwningShard*) override {}
    void visit(const ExpressionInternalIndexKey*) override {}
    void visit(const ExpressionInternalKeyStringValue*) override {}
    void visit(const ExpressionCreateUUID*) override {}
    void visit(const ExpressionCreateObjectId*) override {}
    void visit(const ExpressionTestFeatureFlagLatest*) override {}
    void visit(const ExpressionTestFeatureFlagLastLTS*) override {}
};
}  // namespace mongo
