/**
 * The `$doingMerge` parameter of the `$group` stage is intended to be an internal parameter. Since
 * we cannot reliably enforce its internal status when authorization is disabled, it is necessary to
 * ensure that the accumulator functions behave correctly, even when used within a $group stage with
 * $doingMerge: true.
 */

(function() {
"use strict";
load("jstests/aggregation/extras/utils.js");

const coll = db[jsTestName()];
coll.drop();

const docs = [
    {groupKey: 1, scalar: 1},
    {groupKey: 1, scalar: 2},
    {groupKey: 1, scalar: 3},
    {groupKey: 2, scalar: 11},
    {groupKey: 2, scalar: 12},
    {groupKey: 2, scalar: 13},
];

assert.commandWorked(coll.insertMany(docs));

function assertTypeMismatch(aggFunction, aggFunctionArgument) {
    const pipeline = [{
        $group: {
            _id: "$groupKey",
            agg: {[aggFunction]: aggFunctionArgument},
            $doingMerge: true,
        }
    }];
    assertErrCodeAndErrMsgContains(coll, pipeline, 9961600, aggFunction);
}

function assertNoError(aggFunction, aggFunctionArgument) {
    const pipeline = [{
        $group: {
            _id: "$groupKey",
            agg: {[aggFunction]: aggFunctionArgument},
            $doingMerge: true,
        }
    }];
    assert.eq(coll.aggregate(pipeline).toArray().length, 2);
}

/////////////////////////////////////////////////////////////////////////////////////////////
// The following accumulator functions validate their input and display an error to the user if
// being used with $doingMerge: true.

assertTypeMismatch("$avg", "$scalar");
assertTypeMismatch("$addToSet", "$scalar");
assertTypeMismatch("$minN", {input: "$scalar", n: 2});
assertTypeMismatch("$maxN", {input: "$scalar", n: 2});
assertTypeMismatch("$bottom", {output: "$scalar", sortBy: {d: 1}});
assertTypeMismatch("$top", {output: "$scalar", sortBy: {d: 1}});
assertTypeMismatch("$bottomN", {output: "$scalar", sortBy: {d: 1}, n: 2});
assertTypeMismatch("$topN", {output: "$scalar", sortBy: {d: 1}, n: 2});
assertTypeMismatch("$percentile", {input: "$scalar", p: [0.5], method: "approximate"});
assertTypeMismatch("$push", "$scalar");
assertTypeMismatch("$stdDevPop", "$scalar");
assertTypeMismatch("$stdDevSamp", "$scalar");

/////////////////////////////////////////////////////////////////////////////////////////////
// The following accumulator functions don't require a different type for merging pass.

assertNoError("$first", "$scalar");
assertNoError("$last", "$scalar");
assertNoError("$min", "$scalar");
assertNoError("$max", "$scalar");

/////////////////////////////////////////////////////////////////////////////////////////////
// $sum in 7.0 ignores numeric input for merging stage

assertNoError("$sum", "$scalar");

/////////////////////////////////////////////////////////////////////////////////////////////
// Window function accumulators do not support merging in the first place:
// $covariancePop, $covarianceSamp, $expMovingAvg, $documentNumber, $rank, $denseRank, $integral,
// $locf.
}());
