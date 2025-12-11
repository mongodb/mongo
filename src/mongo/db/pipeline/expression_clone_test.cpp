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

#include "mongo/base/initializer.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"

#include <algorithm>

#include <boost/algorithm/string/join.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {
std::vector<BSONObj> testExpressions;

MONGO_INITIALIZER_GENERAL(InitExpressionsForCloneTest, ("EndExpressionRegistration"), ())
(InitializerContext*) {
    testExpressions = {
        fromjson("{$abs: -1}"),
        fromjson("{$add: ['$foo', 5]}"),
        fromjson("{$allElementsTrue: ['$foo']}"),
        fromjson("{$and: [true, {$eq: ['$foo', 1]}]}"),
        fromjson("{$anyElementTrue: ['$foo']}"),
        fromjson("{$arrayElemAt: ['$foo', 0]}"),
        fromjson("{$arrayToObject: '$foo'}"),
        fromjson("{$avg: ['$foo', 42]}"),
        fromjson("{$ceil: '$foo'}"),
        fromjson("{$cmp: [0, 1]}"),
        fromjson("{$concat: ['str', '$foo']}"),
        fromjson("{$concatArrays: [[], '$foo']}"),
        fromjson(R"({
            $convert: {
                input: '$foo', 
                to: 'binData', 
                format: 'uuid', 
                onError: 'xyz', 
                onNull: 'abc'
            }
        })"),
        fromjson("{$dateAdd: {startDate : '$foo', unit : 'day', amount : 1, timezone: '$foo'}}"),
        fromjson(
            "{$dateDiff: {startDate: '$foo', endDate: '$foo', unit: '$foo', timezone: '$foo'}}"),
        fromjson(R"({
            $dateFromParts: {
                year: '$year', 
                month: '$month', 
                day: '$day', 
                hour: '$hour', 
                minute: '$minute', 
                second: '$second', 
                millisecond: '$ms', 
                timezone: '$tz'
            }
        })"),
        fromjson("{$dateToParts: {date: '$foo', timezone: '$tz', iso8601: '$x'}}"),
        fromjson(R"({
            $dateFromString: {
                dateString: '$foo', 
                timezone: '$tz', 
                format: '$format', 
                onNull: 'null', 
                onError: 'error'
            }
        })"),
        fromjson(
            "{$dateSubtract: {startDate : '$foo', unit : 'day', amount : 1, timezone: '$foo'}}"),
        fromjson(
            "{$dateToString: {date: '$foo', format: '$format', onNull: 'null', timezone: '$tz'}}"),
        fromjson(R"({
            $dateTrunc: {
                date: '$foo', 
                unit: '$foo', 
                binSize: '$foo', 
                timezone: '$foo', 
                startOfWeek: '$foo'
            }
        })"),
        fromjson("{$dayOfMonth: {date: '$foo', timezone: '$tz'}}"),
        fromjson("{$dayOfWeek: {date: '$foo', timezone: '$tz'}}"),
        fromjson("{$divide: ['$foo', 42.0]}"),
        fromjson("{$encStrStartsWith: {input: \"$foo\", prefix:\"21\"}}"),
        fromjson("{$encStrContains: {input: \"$foo\", substring:\"21\"}}"),
        fromjson("{$encStrEndsWith: {input: \"$foo\", suffix:\"21\"}}"),
        fromjson("{$encStrNormalizedEq: {input: \"$foo\", string:\"21\"}}"),
        fromjson("{$eq: ['$foo', '8675309']}"),
        fromjson("{$exp: '$foo'}"),
        fromjson("{$floor: '$foo'}"),
        fromjson("{$function: {body: 'function(){}', args: ['$foo'], lang: 'js'}}"),
        fromjson("{$gt: [5, '$foo']}"),
        fromjson("{$gte: ['$a', '$foo']}"),
        fromjson("{$hour: {date: '$foo', timezone: '$tz'}}"),
        fromjson("{$in: ['$foo', [1, 2, 3]]}"),
        fromjson("{$indexOfArray: ['$foo', 42.0]}"),
        fromjson("{$indexOfBytes: ['$foo', '1234']}"),
        fromjson("{$indexOfCP: ['$foo', '1234']}"),
        BSON("$_internalJsEmit" << BSON("this" << "{}"
                                               << "eval" << BSONCode("function(){}"))),
        fromjson("{$isArray: '$foo'}"),
        fromjson("{$isoDayOfWeek: {date: '$foo', timezone: '$tz'}}"),
        fromjson("{$isoWeek: {date: '$foo', timezone: '$tz'}}"),
        fromjson("{$isoWeekYear: {date: '$foo', timezone: '$tz'}}"),
        fromjson("{$literal: '$foo'}"),
        fromjson("{$ln: '$foo'}"),
        fromjson("{$log: ['$foo', 10]}"),
        fromjson("{$log10: '$foo'}"),
        fromjson("{$lt: ['$foo', 42]}"),
        fromjson("{$lte: ['$foo', 42]}"),
        fromjson("{$ltrim: {input: '$foo', chars: '-'}}"),
        fromjson("{$map: {input: '$foo', as: 'elem', in: {$eq: ['$elem', 42]}}}"),
        fromjson("{$max: ['$foo', 42]}"),
        fromjson("{$mergeObjects: ['$foo', null]}"),
        fromjson("{$meta: 'textScore'}"),
        fromjson("{$min: ['$foo', 42]}"),
        fromjson("{$millisecond: {date: '$foo', timezone: '$tz'}}"),
        fromjson("{$minute: {date: '$foo', timezone: '$tz'}}"),
        fromjson("{$mod: ['$foo', 42]}"),
        fromjson("{$month: {date: '$foo', timezone: '$tz'}}"),
        fromjson("{$multiply: ['$foo', 42]}"),
        fromjson("{$ne: ['$foo', 42]}"),
        fromjson("{$not: ['$foo']}"),
        fromjson("{$objectToArray: '$foo'}"),
        fromjson("{$or: [false, '$foo']}"),
        fromjson("{$pow: ['$foo', 0]}"),
        fromjson("{$range: ['$foo', 42, 1]}"),
        fromjson("{$reverseArray: '$foo'}"),
        fromjson("{$rtrim: {input: '$foo', chars: '-'}}"),
        fromjson("{$second: {date: '$foo', timezone: '$tz'}}"),
        fromjson("{$setDifference: [[1, 2, 3], '$foo']}"),
        fromjson("{$setEquals: [[1, 2, 3], '$foo']}"),
        fromjson("{$setIntersection: [[1, 2, 3], '$foo']}"),
        fromjson("{$setIsSubset: [[1, 2, 3], '$foo']}"),
        fromjson("{$setUnion: [[1, 2, 3], '$foo']}"),
        fromjson("{$size: '$foo'}"),
        fromjson("{$slice: [[1, 2, 3], 1, 1]}"),
        fromjson("{$split: ['$foo', '-']}"),
        fromjson("{$sqrt: '$foo'}"),
        fromjson("{$strcasecmp: ['$foo', '42']}"),
        fromjson("{$strLenBytes: '$foo'}"),
        fromjson("{$strLenCP: '$foo'}"),
        fromjson("{$substrBytes: ['abcde', 1, 2]}"),
        fromjson("{$substrCP: ['abcde', 1, 2]}"),
        fromjson("{$subtract: [42, '$foo']}"),
        fromjson("{$sum: ['$foo', 42]}"),
        fromjson("{$toBool: '$foo'}"),
        fromjson("{$toDate: '$foo'}"),
        fromjson("{$toDecimal: '$foo'}"),
        fromjson("{$toDouble: '$foo'}"),
        fromjson("{$toInt: '$foo'}"),
        fromjson("{$toLong: '$foo'}"),
        fromjson("{$toObjectId: '$foo'}"),
        fromjson("{$toString: '$foo'}"),
        fromjson("{$toLower: '$foo'}"),
        fromjson("{$toUpper: '$foo'}"),
        fromjson("{$trim: {input: '$foo', chars: '-'}}"),
        fromjson("{$trunc: '$foo'}"),
        fromjson("{$type: '$foo'}"),
        fromjson("{$week: {date: '$foo', timezone: '$tz'}}"),
        fromjson("{$year: {date: '$foo', timezone: '$tz'}}"),
        fromjson("{$zip: {inputs: [['a'], ['b'], ['c']]}}"),
        fromjson("{$_internalFindAllValuesAtPath: 'foo'}"),
        fromjson(R"({
            "$_internalFleBetween": {
                "field": '$foo',
                "server": [
                    {
                        "$binary": {
                            "base64": "CKPIq22z0nimnLE48v2/ZyeljOYmlDVlUYEWiCINEhII",
                            "subType": "6"
                        }
                    }
                ]
            }
        })"),
        fromjson(R"({
            "$_internalFleEq": {
                "field": '$foo',
                "server": {
                    "$binary": {
                        "base64": "CCL2XXIiI5N4wjgvCxYp6XRXY1OS39OuX6WT6Y60cR9R",
                        "subType": "6"
                    }
                }
            }
        })"),
        fromjson("{$_internalIndexKey: {doc: '$foo', spec: {key: {a: 1}, name: 'bar'}}}"),
        fromjson(
            "{$_internalKeyStringValue: {input: '$foo', collation: {locale: 'en', strength: 1}}}"),
        fromjson(
            "{$_internalOwningShard: {shardKeyVal: {_id: '$_id'}, ns: 'foo', shardVersion: 1}}"),
        fromjson("{$_internalSortKey: {}}"),
        fromjson("{$_testApiVersion: {unstable: true}}"),
        fromjson("{$_testApiVersion: {deprecated: true}}"),
        fromjson("{$_testFeatureFlagLastLTS: 1}"),
        fromjson("{$_testFeatureFlagLatest: 1}"),
        fromjson("{$acos: '$foo'}"),
        fromjson("{$acosh: '$foo'}"),
        fromjson("{$asin: '$foo'}"),
        fromjson("{$asinh: '$foo'}"),
        fromjson("{$atan: '$foo'}"),
        fromjson("{$atan2: ['$foo', '$bar']}"),
        fromjson("{$atanh: '$foo'}"),
        fromjson("{$binarySize: 'foo'}"),
        fromjson("{$bitAnd: [ '$a', '$b' ]}"),
        fromjson("{$bitNot: '$a'}"),
        fromjson("{$bitOr: [ '$a', '$b' ]}"),
        fromjson("{$bitXor: [ '$a', '$b' ]}"),
        fromjson("{$bsonSize: '$foo'}"),
        fromjson("{$bottom: {input: '$foo', sortBy: 1}}"),
        fromjson("{$bottomN: {input: '$foo', n: 2, sortBy: 1}}"),
        fromjson("{$cond: { if: { $gte: [ '$foo', 250 ] }, then: 30, else: 20 }}"),
        fromjson("{$const: '$foo'}"),
        fromjson("{$cos: '$foo'}"),
        fromjson("{$cosh: '$foo'}"),
        fromjson("{$createObjectId: {}}"),
        fromjson("{$createUUID: {}}"),
        fromjson("{$currentDate: {}}"),
        fromjson("{$dayOfYear: {date: '$foo', timezone: '$tz'}}"),
        fromjson("{$degreesToRadians: '$foo'}"),
        fromjson("{$expr: {$add: ['$a', '$b']}}"),
        fromjson("{$filter: {input: '$foo', as: 'bar', cond: {isNumber: '$$bar'}, limit: 2}}"),
        fromjson("{$first: '$foo'}"),
        fromjson("{$firstN: {input: '$foo', n: 2}}"),
        fromjson("{$getField: {field: '$foo', input: '$bar'}}"),
        fromjson("{$ifNull: ['$a', '$b', '$c']}"),
        fromjson("{$isNumber: '$foo'}"),
        fromjson("{$last: '$foo'}"),
        fromjson("{$lastN: {input: '$foo', n: 2}}"),
        fromjson("{$let: {vars: { a: 1, b: '$foo', c: '$bar' }, in: { $gt: [ '$$a', '$$b' ] }}}"),
        fromjson("{$maxN: {input: '$foo', n: 2}}"),
        fromjson("{$median: {input: '$foo', method: 'approximate'}}"),
        fromjson("{$minN: {input: '$foo', n: 2}}"),
        fromjson("{$percentile: {input: '$foo', p: [ 0.95 ], method: 'approximate'}}"),
        fromjson("{$radiansToDegrees: '$foo'}"),
        fromjson("{$rand: {}}"),
        fromjson(R"({
            $reduce: {
                input: '$foo', 
                initialValue: 1, 
                in: { $multiply: [ '$$value', '$$this' ]}
            }
        })"),
        fromjson("{$regexFind: {input: '$foo', regex: /abc/, options: 'i'}}"),
        fromjson("{$regexFindAll: {input: '$foo', regex: /abc/, options: 'i'}}"),
        fromjson("{$regexMatch: {input: '$foo', regex: /abc/, options: 'i'}}"),
        fromjson("{$replaceAll: {input: '$foo', find: 'abc', replacement: 'bar'}}"),
        fromjson("{$replaceOne: {input: '$foo', find: 'abc', replacement: 'bar'}}"),
        fromjson("{$round: [ '$foo', 0 ]}"),
        fromjson("{$setField: {field: 'foo', input: '$bar', value: 'abc'}}"),
        fromjson("{$similarityCosine: {vectors: [ [1, 2] , [3, 4] ], score: true}}"),
        fromjson("{$similarityDotProduct: {vectors: [ [1, 2, 3] , [4, 5, 6] ], score: true}}"),
        fromjson("{$similarityEuclidean: {vectors: [ [1, 2, 3] , [4, 5, 6] ], score: true}}"),
        fromjson("{$sin: '$foo'}"),
        fromjson("{$sinh: '$foo'}"),
        fromjson("{$sortArray: {input: '$foo', sortBy: { name: 1 }}}"),
        fromjson("{$stdDevPop: ['$a', '$b']}"),
        fromjson("{$stdDevSamp: ['$a', '$b']}"),
        fromjson("{$substr: ['$foo', 0, 2 ]}"),
        fromjson("{$subtype: '$foo'}"),
        fromjson(R"({
            $switch: {
                branches: [
                    {case: {$eq: ['$foo', 1]}, then: 'foo'}, 
                    {case: {$eq: ['$bar', 0]}, then: 'bar'}
                ], 
                default: null
            }
        })"),
        fromjson("{$tan: '$foo'}"),
        fromjson("{$tanh: '$foo'}"),
        fromjson("{$top: {input: '$foo', sortBy: 1}}"),
        fromjson("{$topN: {input: '$foo', n: 2, sortBy: 1}}"),
        fromjson("{$toHashedIndexKey: '$foo'}"),
        fromjson("{$toUUID: '$foo'}"),
        fromjson("{$tsIncrement: '$foo'}"),
        fromjson("{$tsSecond: '$foo'}"),
        fromjson("{$unsetField: {field: 'bar', input: '$foo'}}"),
        fromjson("{$toArray: '$foo'}"),
        fromjson("{$toObject: '$foo'}"),
        fromjson("{$serializeEJSON: {input: '$foo'}}"),
        fromjson("{$deserializeEJSON: {input: '$foo'}}"),
    };

    // Some expressions in the list above may be disable for one reason or another (e.g., they can
    // be hidden behind a feature flag which is not enabled yet). We will remove such expressions
    // from the test run, but we still would want to have them in the list, as at some point the
    // feature flag will get enabled and we will start testing them.
    stdx::unordered_set<std::string> disabledExpressions = Expression::listDisabledExpressions();
    for (auto it = testExpressions.begin(); it != testExpressions.end();) {
        if (disabledExpressions.count(it->firstElement().fieldName()) > 0) {
            it = testExpressions.erase(it);
        } else {
            ++it;
        }
    }
}

std::pair<boost::intrusive_ptr<Expression>, BSONObj> makeExpression(ExpressionContext* expCtx,
                                                                    BSONObj exprSpec) {
    auto expr = BSON("expr" << exprSpec);
    return {Expression::parseOperand(expCtx, expr["expr"], expCtx->variablesParseState), expr};
}

TEST(ExpressionCloneTest, AllRegisteredExpressionsHaveATest) {
    stdx::unordered_set<std::string> registeredExpressions =
        Expression::listRegisteredExpressions();

    // $sigmoid is just a syntactic sugar for some special $divide expression. We do register a
    // parser for it, but there is no a real $sigmoid expression to test.
    registeredExpressions.erase("$sigmoid");

    for (auto&& expr : testExpressions) {
        registeredExpressions.erase(expr.firstElement().fieldName());
    }

    ASSERT_TRUE(registeredExpressions.empty())
        << "The following expressions don't have a test for the 'clone' method: "
        << boost::algorithm::join(registeredExpressions, ", ");
}

TEST(ExpressionCloneTest, SerializedClonedExpressionIsEquivalentToOriginal) {
    auto exprCtx = make_intrusive<ExpressionContextForTest>();

    for (auto&& exprSpec : testExpressions) {
        auto&& [clonedExpr, origExprSerialized, backedBson] =
            [&]() -> std::tuple<boost::intrusive_ptr<Expression>, Value, BSONObj> {
            auto&& [origExpr, backedBson] = makeExpression(exprCtx.get(), exprSpec);
            // Upon retruning from this lambda the 'origExpression' should be destroyed, as it's not
            // shared with any other expression. If the cloned expression somehow keeps raw pointers
            // or references to the original expression tree, the test should fail.
            return {origExpr->clone(), origExpr->serialize(), backedBson};
        }();

        ASSERT_TRUE(ValueComparator().evaluate(origExprSerialized == clonedExpr->serialize()))
            << "Mismatch between cloned and original expressions: " << exprSpec;
    }
}

}  // namespace
}  // namespace mongo
