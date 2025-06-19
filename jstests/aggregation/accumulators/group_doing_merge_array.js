/**
 * The `$doingMerge` parameter of the `$group` stage is intended to be an internal parameter. Since
 * we cannot reliably enforce its internal status when authorization is disabled, it is necessary to
 * ensure that the accumulator functions behave correctly, even when used within a $group stage with
 * $doingMerge: true.
 * This suits tests array accumulator functions which were enabled for $group
 * in 8.1. TODO SERVER-97514: Once featureFlagArrayAccumulators is removed the tests can be moved to
 * the main file: group_doing_merge.js.
 *
 * @tags: [
 *   requires_fcv_81,
 *   featureFlagArrayAccumulators,
 * ]
 */

const coll = db[jsTestName()];
coll.drop();

const docs = [
    {groupKey: 1, array: [1, 2, 3]},
    {groupKey: 1, array: [11, 12, 13]},
    {groupKey: 1, array: [21, 22, 23]},
    {groupKey: 2, array: [31, 32, 33]},
    {groupKey: 2, array: [41, 42, 43]},
    {groupKey: 2, array: [51, 52, 53]},
];

assert.commandWorked(coll.insertMany(docs));

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
// Array accumulator functions do not display errors since they already take their inputs as arrays.

assertNoError("$concatArrays", "$array");
assertNoError("$setUnion", "$array");
