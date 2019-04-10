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

#include "mongo/platform/basic.h"

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
class ExpressionObjectToArray;
class ExpressionArrayToObject;
class ExpressionCeil;
class ExpressionCoerceToBool;
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
class ExpressionHour;
class ExpressionIfNull;
class ExpressionIn;
class ExpressionIndexOfArray;
class ExpressionIndexOfBytes;
class ExpressionIndexOfCP;
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
class ExpressionSetDifference;
class ExpressionSetEquals;
class ExpressionSetIntersection;
class ExpressionSetIsSubset;
class ExpressionSetUnion;
class ExpressionSize;
class ExpressionReverseArray;
class ExpressionSlice;
class ExpressionIsArray;
class ExpressionRound;
class ExpressionSecond;
class ExpressionSplit;
class ExpressionSqrt;
class ExpressionStrcasecmp;
class ExpressionSubstrBytes;
class ExpressionSubstrCP;
class ExpressionStrLenBytes;
class ExpressionStrLenCP;
class ExpressionSubtract;
class ExpressionSwitch;
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
class ExpressionDegreesToRadians;
class ExpressionRadiansToDegrees;

class AccumulatorAvg;
class AccumulatorMax;
class AccumulatorMin;
class AccumulatorStdDevPop;
class AccumulatorStdDevSamp;
class AccumulatorSum;
class AccumulatorMergeObjects;
template <typename Accumulator>
class ExpressionFromAccumulator;

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
 */
class ExpressionVisitor {
public:
    virtual ~ExpressionVisitor() = default;
    virtual void visit(ExpressionConstant*) = 0;
    virtual void visit(ExpressionAbs*) = 0;
    virtual void visit(ExpressionAdd*) = 0;
    virtual void visit(ExpressionAllElementsTrue*) = 0;
    virtual void visit(ExpressionAnd*) = 0;
    virtual void visit(ExpressionAnyElementTrue*) = 0;
    virtual void visit(ExpressionArray*) = 0;
    virtual void visit(ExpressionArrayElemAt*) = 0;
    virtual void visit(ExpressionObjectToArray*) = 0;
    virtual void visit(ExpressionArrayToObject*) = 0;
    virtual void visit(ExpressionCeil*) = 0;
    virtual void visit(ExpressionCoerceToBool*) = 0;
    virtual void visit(ExpressionCompare*) = 0;
    virtual void visit(ExpressionConcat*) = 0;
    virtual void visit(ExpressionConcatArrays*) = 0;
    virtual void visit(ExpressionCond*) = 0;
    virtual void visit(ExpressionDateFromString*) = 0;
    virtual void visit(ExpressionDateFromParts*) = 0;
    virtual void visit(ExpressionDateToParts*) = 0;
    virtual void visit(ExpressionDateToString*) = 0;
    virtual void visit(ExpressionDivide*) = 0;
    virtual void visit(ExpressionExp*) = 0;
    virtual void visit(ExpressionFieldPath*) = 0;
    virtual void visit(ExpressionFilter*) = 0;
    virtual void visit(ExpressionFloor*) = 0;
    virtual void visit(ExpressionIfNull*) = 0;
    virtual void visit(ExpressionIn*) = 0;
    virtual void visit(ExpressionIndexOfArray*) = 0;
    virtual void visit(ExpressionIndexOfBytes*) = 0;
    virtual void visit(ExpressionIndexOfCP*) = 0;
    virtual void visit(ExpressionLet*) = 0;
    virtual void visit(ExpressionLn*) = 0;
    virtual void visit(ExpressionLog*) = 0;
    virtual void visit(ExpressionLog10*) = 0;
    virtual void visit(ExpressionMap*) = 0;
    virtual void visit(ExpressionMeta*) = 0;
    virtual void visit(ExpressionMod*) = 0;
    virtual void visit(ExpressionMultiply*) = 0;
    virtual void visit(ExpressionNot*) = 0;
    virtual void visit(ExpressionObject*) = 0;
    virtual void visit(ExpressionOr*) = 0;
    virtual void visit(ExpressionPow*) = 0;
    virtual void visit(ExpressionRange*) = 0;
    virtual void visit(ExpressionReduce*) = 0;
    virtual void visit(ExpressionSetDifference*) = 0;
    virtual void visit(ExpressionSetEquals*) = 0;
    virtual void visit(ExpressionSetIntersection*) = 0;
    virtual void visit(ExpressionSetIsSubset*) = 0;
    virtual void visit(ExpressionSetUnion*) = 0;
    virtual void visit(ExpressionSize*) = 0;
    virtual void visit(ExpressionReverseArray*) = 0;
    virtual void visit(ExpressionSlice*) = 0;
    virtual void visit(ExpressionIsArray*) = 0;
    virtual void visit(ExpressionRound*) = 0;
    virtual void visit(ExpressionSplit*) = 0;
    virtual void visit(ExpressionSqrt*) = 0;
    virtual void visit(ExpressionStrcasecmp*) = 0;
    virtual void visit(ExpressionSubstrBytes*) = 0;
    virtual void visit(ExpressionSubstrCP*) = 0;
    virtual void visit(ExpressionStrLenBytes*) = 0;
    virtual void visit(ExpressionStrLenCP*) = 0;
    virtual void visit(ExpressionSubtract*) = 0;
    virtual void visit(ExpressionSwitch*) = 0;
    virtual void visit(ExpressionToLower*) = 0;
    virtual void visit(ExpressionToUpper*) = 0;
    virtual void visit(ExpressionTrim*) = 0;
    virtual void visit(ExpressionTrunc*) = 0;
    virtual void visit(ExpressionType*) = 0;
    virtual void visit(ExpressionZip*) = 0;
    virtual void visit(ExpressionConvert*) = 0;
    virtual void visit(ExpressionRegexFind*) = 0;
    virtual void visit(ExpressionRegexFindAll*) = 0;
    virtual void visit(ExpressionRegexMatch*) = 0;
    virtual void visit(ExpressionCosine*) = 0;
    virtual void visit(ExpressionSine*) = 0;
    virtual void visit(ExpressionTangent*) = 0;
    virtual void visit(ExpressionArcCosine*) = 0;
    virtual void visit(ExpressionArcSine*) = 0;
    virtual void visit(ExpressionArcTangent*) = 0;
    virtual void visit(ExpressionArcTangent2*) = 0;
    virtual void visit(ExpressionHyperbolicArcTangent*) = 0;
    virtual void visit(ExpressionHyperbolicArcCosine*) = 0;
    virtual void visit(ExpressionHyperbolicArcSine*) = 0;
    virtual void visit(ExpressionHyperbolicTangent*) = 0;
    virtual void visit(ExpressionHyperbolicCosine*) = 0;
    virtual void visit(ExpressionHyperbolicSine*) = 0;
    virtual void visit(ExpressionDegreesToRadians*) = 0;
    virtual void visit(ExpressionRadiansToDegrees*) = 0;
    virtual void visit(ExpressionDayOfMonth*) = 0;
    virtual void visit(ExpressionDayOfWeek*) = 0;
    virtual void visit(ExpressionDayOfYear*) = 0;
    virtual void visit(ExpressionHour*) = 0;
    virtual void visit(ExpressionMillisecond*) = 0;
    virtual void visit(ExpressionMinute*) = 0;
    virtual void visit(ExpressionMonth*) = 0;
    virtual void visit(ExpressionSecond*) = 0;
    virtual void visit(ExpressionWeek*) = 0;
    virtual void visit(ExpressionIsoWeekYear*) = 0;
    virtual void visit(ExpressionIsoDayOfWeek*) = 0;
    virtual void visit(ExpressionIsoWeek*) = 0;
    virtual void visit(ExpressionYear*) = 0;
    virtual void visit(ExpressionFromAccumulator<AccumulatorAvg>*) = 0;
    virtual void visit(ExpressionFromAccumulator<AccumulatorMax>*) = 0;
    virtual void visit(ExpressionFromAccumulator<AccumulatorMin>*) = 0;
    virtual void visit(ExpressionFromAccumulator<AccumulatorStdDevPop>*) = 0;
    virtual void visit(ExpressionFromAccumulator<AccumulatorStdDevSamp>*) = 0;
    virtual void visit(ExpressionFromAccumulator<AccumulatorSum>*) = 0;
    virtual void visit(ExpressionFromAccumulator<AccumulatorMergeObjects>*) = 0;
    virtual void visit(ExpressionTests::Testable*) = 0;
};

}  // namespace mongo
