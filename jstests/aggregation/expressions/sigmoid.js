/**
 * Tests the $sigmoid aggregation expression.
 * @tags: [ featureFlagRankFusionBasic, requires_fcv_81 ]
 */
import {anyEq, assertErrorCode, testExpression} from "jstests/aggregation/extras/utils.js";

const coll = db.getCollection(jsTestName());
coll.drop();

// Test $sigmoid on numeric inputs directly.
testExpression(coll, {$sigmoid: 0}, 0.5);
testExpression(coll, {$sigmoid: 5}, 0.9933071490757153);
testExpression(coll, {$sigmoid: 10.11}, 0.999959330847226);
testExpression(coll, {$sigmoid: -24}, 3.7751345441365816e-11);
testExpression(coll,
               {$sigmoid: NumberDecimal("-3.14")},
               NumberDecimal("0.04148711930169585060767831941324486"));
testExpression(
    coll, {$sigmoid: NumberDecimal("3.14")}, NumberDecimal("0.9585128806983041493923216805867550"));
testExpression(coll, {$sigmoid: 100}, 1);
testExpression(coll, {$sigmoid: NumberInt("-2147483648")}, 0);
testExpression(coll, {$sigmoid: NumberInt("2147483647")}, 1);
testExpression(coll, {$sigmoid: NumberLong("-9223372036854775808")}, 0);
testExpression(coll, {$sigmoid: NumberLong("9223372036854775807")}, 1);
coll.drop();

// Test $sigmoid on field paths that evaluate to numeric input.
const testNumericFieldPathDocs = [
    {_id: 1, foo: 0},
    {_id: 2, foo: 1},
    {_id: 3, foo: 2},
    {_id: 4, foo: 3},
    {_id: 5, foo: -4},
    {_id: 6, foo: -5},
    {_id: 7, foo: -100},
    {_id: 8, foo: 100},
    {_id: 9, foo: NumberInt(-1)},
    {_id: 10, foo: NumberInt("-2147483648")},
    {_id: 11, foo: NumberInt("2147483647")},
    {_id: 12, foo: NumberLong("-9223372036854775808")},
    {_id: 13, foo: NumberLong("9223372036854775807")},
    {_id: 14, foo: NumberDecimal("-3.14")},
    {_id: 15, foo: NumberDecimal("3.14")},
];
const expectedNumericFieldPathResults = [
    {_id: 1, computed: 0.5},
    {_id: 2, computed: 0.7310585786300049},
    {_id: 3, computed: 0.8807970779778823},
    {_id: 4, computed: 0.9525741268224334},
    {_id: 5, computed: 0.01798620996209156},
    {_id: 6, computed: 0.0066928509242848554},
    {_id: 7, computed: 3.7200759760208356e-44},
    {_id: 8, computed: 1},
    {_id: 9, computed: 0.2689414213699951},
    {_id: 10, computed: 0},
    {_id: 11, computed: 1},
    {_id: 12, computed: 0},
    {_id: 13, computed: 1},
    {_id: 14, computed: NumberDecimal("0.04148711930169585060767831941324486")},
    {_id: 15, computed: NumberDecimal("0.9585128806983041493923216805867550")},
];

coll.insertMany(testNumericFieldPathDocs);
const numericFieldPathResults =
    coll.aggregate([{$project: {computed: {$sigmoid: "$foo"}}}]).toArray();
assert(anyEq(numericFieldPathResults, expectedNumericFieldPathResults));
coll.drop();

// Test $sigmoid on legal nested expressions.
assert.commandWorked(coll.insertMany([
    {_id: 1, length: 1, height: 1, width: 1},
    {_id: 2, length: 4, height: 3, width: 9},
    {_id: 3, length: 2.3, height: 4.9, width: 2.1},
    {_id: 4, length: 12345, height: 678910, width: 2.1}
]));
const numericNestedExpressionResults =
    coll.aggregate([{
            $project: {
                _id: 1,
                perimeterSigmoid: {$sigmoid: {$add: ["$length", "$width"]}},
                volumeSigmoid: {$sigmoid: {$multiply: ["$length", "$width", "$height"]}}
            }
        }])
        .toArray();
const expectedNumericNestedExpressionResults = [
    {_id: 1, perimeterSigmoid: 0.8807970779778823, volumeSigmoid: 0.7310585786300049},
    {_id: 2, perimeterSigmoid: 0.999997739675702, volumeSigmoid: 1},
    {
        _id: 3,
        perimeterSigmoid: 0.9878715650157257,
        volumeSigmoid: 0.9999999999473312,
    },
    {_id: 4, perimeterSigmoid: 1, volumeSigmoid: 1}
];

assert(anyEq(numericNestedExpressionResults, expectedNumericNestedExpressionResults));
coll.drop();

// Test bad input to $sigmoid that will be caught directly by $sigmoid.
assertErrorCode(coll, {$project: {computed: {$sigmoid: "4"}}}, ErrorCodes.TypeMismatch);
assertErrorCode(coll, {$project: {computed: {$sigmoid: "kyra"}}}, ErrorCodes.TypeMismatch);
assertErrorCode(coll,
                {$project: {computed: {$sigmoid: ["claudia", "will", "reilly", "ted"]}}},
                ErrorCodes.TypeMismatch);
assertErrorCode(coll, {$project: {computed: {$sigmoid: [1, 2, 3, 4]}}}, ErrorCodes.TypeMismatch);
// TODO SERVER-92973: Ensure that the errors below will be caught by $sigmoid and not the desugared
// $multiply.
assertErrorCode(coll, {$project: {computed: {$sigmoid: {$gt: [1, 200]}}}}, ErrorCodes.TypeMismatch);
assertErrorCode(coll,
                {$project: {computed: {$sigmoid: {$concat: ["claudia", "will", "reilly", "ted"]}}}},
                ErrorCodes.TypeMismatch);
assertErrorCode(
    coll, {$project: {computed: {$sigmoid: {$toUpper: "taqi"}}}}, ErrorCodes.TypeMismatch);
