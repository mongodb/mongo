(function() {
"use strict";
load("jstests/aggregation/extras/utils.js");        // For assertErrorCode and assertErrMsgContains.
load("jstests/libs/sbe_assert_error_override.js");  // Override error-code-checking APIs.

// In SERVER-63012, translation of $add expression into sbe now defaults the translation of $add
// with no operands to a zero integer constant.
const coll = db.add_coll;
coll.drop();

assert.commandWorked(coll.insert({x: 1}));
let result = coll.aggregate([{$project: {y: {$add: []}}}]).toArray();
assert.eq(result[0]["y"], 0);

// Confirm that we're not using DoubleDoubleSummation for $add expression with a set of double
// values.
let arr = [
    1.4831356930199802e-05, -3.121724665346865,     3041897608700.073,       1001318343149.7166,
    -1714.6229586696593,    1731390114894580.8,     6.256645803154374e-08,   -107144114533844.25,
    -0.08839485091750919,   -265119153.02185738,    -0.02450615965231944,    0.0002684331017079073,
    32079040427.68358,      -0.04733295911845742,   0.061381859083076085,    -25329.59126796951,
    -0.0009567520620034965, -1553879364344.9932,    -2.1101077525869814e-08, -298421079729.5547,
    0.03182394834273594,    22.201944843278916,     -33.35667991109125,      11496013.960449915,
    -40652595.33210472,     3.8496066090328163,     2.5074042398147304e-08,  -0.02208724071782122,
    -134211.37290639878,    0.17640433666616578,    4.463787499171126,       9.959669945399718,
    129265976.35224283,     1.5865526187526546e-07, -4746011.710555799,      -712048598925.0789,
    582214206210.4034,      0.025236204812875362,   530078170.91147506,      -14.865307666195053,
    1.6727994895185032e-05, -113386276.03121366,    -6.135827207137054,      10644945799901.145,
    -100848907797.1582,     2.2404406961625282e-08, 1.315662618424494e-09,   -0.832190208349044,
    -9.779323414999364,     -546522170658.2997
];
let doc = {_id: 0};
let i = 0;
let queryArr = [];
arr.forEach(num => {
    i++;
    doc[`f_${i}`] = num;
    queryArr.push(`$f_${i}`);
});

assert.eq(true, coll.drop());
assert.commandWorked(coll.insert(doc));

let addResult = coll.aggregate([{$project: {add: {$add: queryArr}}}]).toArray();
let sumResult = coll.aggregate([{$project: {sum: {$sum: queryArr}}}]).toArray();
assert.neq(addResult[0]["add"], sumResult[0]["sum"]);
assert.eq(addResult[0]["add"], arr.reduce((a, b) => a + b));

assert.eq(true, coll.drop());
// Doubles are rounded to int64 when added to Date
assert.commandWorked(coll.insert({_id: 0, lhs: new Date(1683794065002), rhs: 0.5}));
assert.commandWorked(coll.insert({_id: 1, lhs: new Date(1683794065002), rhs: 1.4}));
assert.commandWorked(coll.insert({_id: 2, lhs: new Date(1683794065002), rhs: 1.5}));
assert.commandWorked(coll.insert({_id: 3, lhs: new Date(1683794065002), rhs: 1.7}));
// Decimals are rounded to int64, when tie rounded to even, when added to Date
assert.commandWorked(
    coll.insert({_id: 4, lhs: new Date(1683794065002), rhs: new NumberDecimal("1.4")}));
assert.commandWorked(
    coll.insert({_id: 5, lhs: new Date(1683794065002), rhs: new NumberDecimal("1.5")}));
assert.commandWorked(
    coll.insert({_id: 6, lhs: new Date(1683794065002), rhs: new NumberDecimal("1.7")}));
assert.commandWorked(
    coll.insert({_id: 7, lhs: new Date(1683794065002), rhs: new NumberDecimal("2.5")}));

let result1 =
    coll.aggregate([{$project: {sum: {$add: ["$lhs", "$rhs"]}}}, {$sort: {_id: 1}}]).toArray();
assert.eq(result1[0].sum, new Date(1683794065003));
assert.eq(result1[1].sum, new Date(1683794065003));
assert.eq(result1[2].sum, new Date(1683794065004));
assert.eq(result1[3].sum, new Date(1683794065004));
assert.eq(result1[4].sum, new Date(1683794065003));
assert.eq(result1[5].sum, new Date(1683794065004));
assert.eq(result1[6].sum, new Date(1683794065004));
assert.eq(result1[7].sum, new Date(1683794065004));

coll.drop();

assert.commandWorked(coll.insert([{
    _id: 0,
    veryBigPositiveLong: NumberLong("9223372036854775806"),
    veryBigPositiveDouble: 9223372036854775806,
    veryBigPositiveDecimal: NumberDecimal("9223372036854775806")
}]));

let pipeline = [{$project: {res: {$add: [new Date(10), "$veryBigPositiveLong"]}}}];
assertErrCodeAndErrMsgContains(coll, pipeline, ErrorCodes.Overflow, "date overflow");

pipeline = [{$project: {res: {$add: [new Date(10), "$veryBigPositiveDouble"]}}}];
assertErrCodeAndErrMsgContains(coll, pipeline, ErrorCodes.Overflow, "date overflow");

pipeline = [{$project: {res: {$add: [new Date(10), "$veryBigPositiveDecimal"]}}}];
assertErrCodeAndErrMsgContains(coll, pipeline, ErrorCodes.Overflow, "date overflow");
}());
