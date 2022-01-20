// Tests the support for disk storage of intermediate results in aggregation.
//
// Run only when pipeline optimization is enabled, otherwise the type of sorter being used can be
// different (NoLimitSort vs TopKSort) causing an aggregation request to fail with different error
// codes.
//
// Some in memory variants will error because this test uses too much memory. As such, we do not
// run this test on in-memory variants.
//
// @tags: [
//   requires_collstats,
//   requires_pipeline_optimization,
// ]
(function() {
'use strict';

load('jstests/libs/fixture_helpers.js');            // For 'FixtureHelpers'
load("jstests/libs/sbe_assert_error_override.js");  // Override error-code-checking APIs.

const coll = db.spill_to_disk;
coll.drop();

const sharded = FixtureHelpers.isSharded(coll);

const memoryLimitMB = sharded ? 200 : 100;

const bigStr = Array(1024 * 1024 + 1).toString();  // 1MB of ','
for (let i = 0; i < memoryLimitMB + 1; i++)
    assert.commandWorked(coll.insert({_id: i, bigStr: i + bigStr, random: Math.random()}));

assert.gt(coll.stats().size, memoryLimitMB * 1024 * 1024);

function test({pipeline, expectedCodes, canSpillToDisk}) {
    // Test that by default we error out if exceeding memory limit.
    assert.commandFailedWithCode(
        db.runCommand({aggregate: coll.getName(), pipeline: pipeline, cursor: {}}), expectedCodes);

    // Test that 'allowDiskUse: false' does indeed prevent spilling to disk.
    assert.commandFailedWithCode(
        db.runCommand(
            {aggregate: coll.getName(), pipeline: pipeline, cursor: {}, allowDiskUse: false}),
        expectedCodes);

    // Test that allowDiskUse only supports bool. In particular, numbers aren't allowed.
    assert.commandFailed(db.runCommand(
        {aggregate: coll.getName(), pipeline: pipeline, cursor: {}, allowDiskUse: 1}));

    // If this command supports spilling to disk, ensure that it will succeed when disk use is
    // allowed.
    let res = db.runCommand(
        {aggregate: coll.getName(), pipeline: pipeline, cursor: {}, allowDiskUse: true});
    if (canSpillToDisk) {
        assert.eq(new DBCommandCursor(coll.getDB(), res).itcount(),
                  coll.count());  // all tests output one doc per input doc
    } else {
        assert.commandFailedWithCode(res, [ErrorCodes.ExceededMemoryLimit, expectedCodes]);
    }
}

assert.commandWorked(db.adminCommand({
    setParameter: 1,
    internalQuerySlotBasedExecutionHashAggApproxMemoryUseInBytesBeforeSpill: 1024
}));
assert.commandWorked(db.adminCommand(
    {setParameter: 1, internalQuerySlotBasedExecutionHashAggMemoryUseSampleRate: 1.0}));

test({
    pipeline: [{$group: {_id: '$_id', bigStr: {$min: '$bigStr'}}}],
    expectedCodes: ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed,
    canSpillToDisk: true
});

// Sorting with _id would use index which doesn't require external sort, so sort by 'random'
// instead.
test({
    pipeline: [{$sort: {random: 1}}],
    expectedCodes: ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed,
    canSpillToDisk: true
});

test({
    pipeline: [{$sort: {bigStr: 1}}],  // big key and value
    expectedCodes: ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed,
    canSpillToDisk: true
});

// Test that sort + large limit won't crash the server (SERVER-10136)
test({
    pipeline: [{$sort: {bigStr: 1}}, {$limit: 1000 * 1000 * 1000}],
    expectedCodes: ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed,
    canSpillToDisk: true
});

// Test combining two external sorts in both same and different orders.
test({
    pipeline: [{$group: {_id: '$_id', bigStr: {$min: '$bigStr'}}}, {$sort: {_id: 1}}],
    expectedCodes: ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed,
    canSpillToDisk: true
});

test({
    pipeline: [{$group: {_id: '$_id', bigStr: {$min: '$bigStr'}}}, {$sort: {_id: -1}}],
    expectedCodes: ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed,
    canSpillToDisk: true
});

test({
    pipeline: [{$group: {_id: '$_id', bigStr: {$min: '$bigStr'}}}, {$sort: {random: 1}}],
    expectedCodes: ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed,
    canSpillToDisk: true
});
test({
    pipeline: [{$sort: {random: 1}}, {$group: {_id: '$_id', bigStr: {$first: '$bigStr'}}}],
    expectedCodes: ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed,
    canSpillToDisk: true
});

// Test accumulating all values into one array. On debug builds we will spill to disk for $group and
// so may hit the group error code before we hit ExceededMemoryLimit.
test({
    pipeline: [{$group: {_id: null, bigArray: {$push: '$bigStr'}}}],
    expectedCodes:
        [ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed, ErrorCodes.ExceededMemoryLimit],
    canSpillToDisk: false
});
test({
    pipeline:
        [{$group: {_id: null, bigArray: {$addToSet: {$concat: ['$bigStr', {$toString: "$_id"}]}}}}],
    expectedCodes:
        [ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed, ErrorCodes.ExceededMemoryLimit],
    canSpillToDisk: false
});

for (const op of ['$firstN', '$lastN', '$minN', '$maxN', '$topN', '$bottomN']) {
    jsTestLog("Testing op " + op);
    let spec = {n: 100000000};
    if (op === '$topN' || op === '$bottomN') {
        spec['sortBy'] = {random: 1};
        spec['output'] = '$bigStr';
    } else {
        // $firstN/$lastN/$minN/$maxN accept 'input'.
        spec['input'] = '$bigStr';
    }

    // By grouping all of the entries in the same group, it is the case that we will either
    // exceed the per group limit for the 'n' family of accumulators, or the total $group
    // limit when disk use is disabled. Hence, we allow both possible error codes. Also note
    // that we configure 'canSpillToDisk' to be false because spilling to disk will not
    // reduce the memory consumption of our group in this case.
    test({
        pipeline: [{$group: {_id: null, bigArray: {[op]: spec}}}],
        expectedCodes:
            [ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed, ErrorCodes.ExceededMemoryLimit],
        canSpillToDisk: false
    });

    // Because each group uses less than the configured limit, but cumulatively they exceed
    // the limit for $group, we only check for 'QueryExceededMemoryLimitNoDiskUseAllowed'
    // when disk use is disabled.
    test({
        pipeline: [{$group: {_id: '$_id', bigArray: {[op]: spec}}}],
        expectedCodes: [ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed],
        canSpillToDisk: true
    });
}
// don't leave large collection laying around
assert(coll.drop());
})();
