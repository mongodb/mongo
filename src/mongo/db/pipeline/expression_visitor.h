// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/expression_walker.h"
#include "mongo/util/modules.h"

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
class ExpressionTopN;
class ExpressionTop;
class ExpressionBottomN;
class ExpressionBottom;
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
class ExpressionSerializeEJSON;
class ExpressionDeserializeEJSON;
class ExpressionHash;

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
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionTopN>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionTop>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionBottomN>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionBottom>) = 0;
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
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionSerializeEJSON>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionDeserializeEJSON>) = 0;
    virtual void visit(expression_walker::MaybeConstPtr<IsConst, ExpressionHash>) = 0;
};

using ExpressionMutableVisitor = ExpressionVisitor<false>;
using ExpressionConstVisitor = ExpressionVisitor<true>;

/**
 * Uses CRTP (Curiously Recurring Template Pattern) to provide default implementations for all
 * visit methods. A derived class can:
 *  - Override specific visit methods for the expression types it cares about.
 *  - Optionally provide a visitDefault method to handle all visit calls not overridden by the
 *    derived class. If visitDefault is absent, such visits are no-ops.
 *
 * Example without visitDefault (visits not overridden in the derived class are no-ops):
 *
 * struct FieldPathVisitor : public SelectiveConstExpressionVisitorBase<FieldPathVisitor> {
 *     // To avoid overloaded-virtual warnings.
 *     using SelectiveConstExpressionVisitorBase<FieldPathVisitor>::visit;
 *
 *     void visit(const ExpressionFieldPath* expr) final {
 *         // logic for what to do with an ExpressionFieldPath.
 *     }
 * };
 *
 * Example with visitDefault (visitDefault runs for any type not overridden in the derived class):
 *
 * struct AddWithDefaultVisitor : public SelectiveConstExpressionVisitorBase<AddWithDefaultVisitor>
 * { using SelectiveConstExpressionVisitorBase<AddWithDefaultVisitor>::visit;
 *
 *     template <typename T>
 *     void visitDefault(const T* expr) {
 *         // logic for what to do with expr by default.
 *     }
 *
 *     void visit(const ExpressionAdd* expr) final {
 *         // logic for what to do with an ExpressionAdd.
 *     }
 * };
 */
template <typename Derived>
struct SelectiveConstExpressionVisitorBase : public ExpressionConstVisitor {
    void visit(const ExpressionConstant* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionAbs* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionAdd* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionAllElementsTrue* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionAnd* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionAnyElementTrue* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionArray* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionArrayElemAt* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionBitAnd* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionBitOr* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionBitXor* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionBitNot* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionFirst* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionLast* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionObjectToArray* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionArrayToObject* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionBsonSize* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionCeil* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionCompare* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionConcat* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionConcatArrays* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionCond* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionDateDiff* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionDateFromString* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionDateFromParts* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionDateToParts* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionDateToString* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionDateTrunc* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionDivide* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionExp* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionFieldPath* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionFilter* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionFloor* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionIfNull* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionIn* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionIndexOfArray* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionIndexOfBytes* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionIndexOfCP* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionIsNumber* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionLet* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionLn* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionLog* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionLog10* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionInternalFLEBetween* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionInternalFLEEqual* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionEncStrStartsWith* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionEncStrEndsWith* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionEncStrContains* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionEncStrNormalizedEq* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionInternalRawSortKey* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionMap* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionMeta* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionMod* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionMultiply* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionNot* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionObject* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionOr* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionPow* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionRange* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionReduce* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionReplaceOne* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionReplaceAll* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionSetDifference* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionSetEquals* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionSetIntersection* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionSetIsSubset* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionSetUnion* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionSimilarityDotProduct* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionSimilarityCosine* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionSimilarityEuclidean* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionSize* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionReverseArray* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionSortArray* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionTopN* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionTop* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionBottomN* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionBottom* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionSlice* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionIsArray* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionInternalFindAllValuesAtPath* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionRound* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionSplit* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionSqrt* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionStrcasecmp* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionSubstrBytes* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionSubstrCP* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionStrLenBytes* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionBinarySize* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionStrLenCP* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionSubtract* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionSubtype* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionSwitch* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionTestApiVersion* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionToLower* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionToUpper* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionTrim* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionTrunc* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionType* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionZip* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionConvert* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionRegexFind* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionRegexFindAll* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionRegexMatch* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionCosine* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionSine* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionTangent* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionArcCosine* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionArcSine* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionArcTangent* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionArcTangent2* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionHyperbolicArcTangent* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionHyperbolicArcCosine* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionHyperbolicArcSine* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionHyperbolicTangent* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionHyperbolicCosine* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionHyperbolicSine* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionDegreesToRadians* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionRadiansToDegrees* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionDayOfMonth* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionDayOfWeek* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionDayOfYear* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionHour* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionMillisecond* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionMinute* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionMonth* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionSecond* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionWeek* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionIsoWeekYear* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionIsoDayOfWeek* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionIsoWeek* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionYear* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionFromAccumulator<AccumulatorAvg>* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionFromAccumulator<AccumulatorMax>* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionFromAccumulator<AccumulatorMin>* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionFromAccumulatorN<AccumulatorFirstN>* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionFromAccumulatorN<AccumulatorLastN>* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionFromAccumulatorN<AccumulatorMaxN>* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionFromAccumulatorN<AccumulatorMinN>* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionFromAccumulatorQuantile<AccumulatorMedian>* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionFromAccumulatorQuantile<AccumulatorPercentile>* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionFromAccumulator<AccumulatorStdDevPop>* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionFromAccumulator<AccumulatorStdDevSamp>* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionFromAccumulator<AccumulatorSum>* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionFromAccumulator<AccumulatorMergeObjects>* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionTests::Testable* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionInternalJsEmit* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionInternalFindSlice* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionInternalFindPositional* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionInternalFindElemMatch* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionFunction* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionRandom* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionCurrentDate* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionToHashedIndexKey* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionDateAdd* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionDateSubtract* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionGetField* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionSetField* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionTsSecond* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionTsIncrement* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionInternalOwningShard* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionInternalIndexKey* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionInternalKeyStringValue* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionCreateUUID* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionCreateObjectId* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionTestFeatureFlagLatest* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionTestFeatureFlagLastLTS* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionSerializeEJSON* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionDeserializeEJSON* expr) override {
        doDefault(expr);
    }
    void visit(const ExpressionHash* expr) override {
        doDefault(expr);
    }

private:
    template <typename T>
    void doDefault(T* expr) {
        if constexpr (requires { static_cast<Derived&>(*this).visitDefault(expr); }) {
            static_cast<Derived&>(*this).visitDefault(expr);
        }
    }
};

}  // namespace mongo
