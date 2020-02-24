// Tests the support for disk storage of intermediate results in aggregation.
//
// Run only when pipeline optimization is enabled, otherwise the type of sorter being used can be
// different (NoLimitSort vs TopKSort) causing an aggregation request to fail with different error
// codes.
// @tags: [requires_pipeline_optimization, requires_collstats]
(function() {
'use strict';

load('jstests/libs/fixture_helpers.js');  // For 'FixtureHelpers'

const coll = db.spill_to_disk;
coll.drop();

const sharded = FixtureHelpers.isSharded(coll);

const memoryLimitMB = sharded ? 200 : 100;

const bigStr = Array(1024 * 1024 + 1).toString();  // 1MB of ','
for (let i = 0; i < memoryLimitMB + 1; i++)
    coll.insert({_id: i, bigStr: i + bigStr, random: Math.random()});

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

// don't leave large collection laying around
coll.drop();
})();
