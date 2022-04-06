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

load("jstests/aggregation/extras/utils.js");
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

function setHashAggParameters(memoryLimit, atLeast) {
    const oldMemoryLimit =
        assert
            .commandWorked(db.adminCommand({
                setParameter: 1,
                internalQuerySlotBasedExecutionHashAggApproxMemoryUseInBytesBeforeSpill: memoryLimit
            }))
            .was;

    const oldAtLeast =
        assert
            .commandWorked(db.adminCommand({
                setParameter: 1,
                internalQuerySlotBasedExecutionHashAggMemoryCheckPerAdvanceAtLeast: atLeast
            }))
            .was;
    return {memoryLimit: oldMemoryLimit, atLeast: oldAtLeast};
}

function testWithHashAggMemoryLimit({pipeline, expectedCodes, canSpillToDisk, memoryLimit}) {
    // If a test sets a specific memory limit, we should do more frequent checks to respect it.
    const oldSettings = setHashAggParameters(memoryLimit, 1 /*atLEast*/);
    try {
        test({pipeline, expectedCodes, canSpillToDisk});
    } finally {
        setHashAggParameters(oldSettings.memoryLimit, oldSettings.atLeast);
    }
}

testWithHashAggMemoryLimit({
    pipeline: [{$group: {_id: '$_id', bigStr: {$min: '$bigStr'}}}],
    expectedCodes: ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed,
    canSpillToDisk: true,
    memoryLimit: 1024
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

// Test spill to disk for $lookup
const localColl = db.lookup_spill_local_hj;
const foreignColl = db.lookup_spill_foreign_hj;

function setupCollections(localRecords, foreignRecords, foreignField) {
    localColl.drop();
    assert.commandWorked(localColl.insert(localRecords));
    foreignColl.drop();
    assert.commandWorked(foreignColl.insert(foreignRecords));
}

function setHashLookupParameters(memoryLimit) {
    const oldMemoryLimit =
        assert
            .commandWorked(db.adminCommand({
                setParameter: 1,
                internalQuerySlotBasedExecutionHashLookupApproxMemoryUseInBytesBeforeSpill:
                    memoryLimit
            }))
            .was;

    return oldMemoryLimit;
}

/**
 * Executes $lookup with multiple records in the local/foreign collections and checks that the "as"
 * field for it contains documents with ids from `idsExpectedToMatch`.
 */
function runTest_MultipleLocalForeignRecords(
    {testDescription, localRecords, localField, foreignRecords, foreignField, idsExpectedToMatch}) {
    setupCollections(localRecords, foreignRecords, foreignField);
    const pipeline = [{
                  $lookup: {
                                from: foreignColl.getName(),
                                localField: localField,
                                foreignField: foreignField,
                                as: "matched"
                            }}];
    const results = localColl.aggregate(pipeline, {allowDiskUse: true}).toArray();
    const explain = localColl.explain().aggregate(pipeline, {allowDiskUse: true});

    assert.eq(localRecords.length, results.length);

    // Extract matched foreign ids from the "matched" field.
    for (let i = 0; i < results.length; i++) {
        const matchedIds = results[i].matched.map(x => (x._id));
        // Order of the elements within the arrays is not significant for 'assertArrayEq'.
        assertArrayEq({
            actual: matchedIds,
            expected: idsExpectedToMatch[i],
            extraErrorMsg: " **TEST** " + testDescription + " " + tojson(explain)
        });
    }
}

function runHashLookupSpill(memoryLimit) {
    const oldSettings = setHashLookupParameters(memoryLimit);

    (function testMultipleMatchesOnSingleLocal() {
        const docs = [
            {_id: 0, no_a: 1},
            {_id: 1, a: 1},
            {_id: 2, a: [1]},
            {_id: 3, a: [[1]]},
            {_id: 4, a: 1},
        ];

        runTest_MultipleLocalForeignRecords({
            testDescription: "Single Local matches multiple foreign docs",
            localRecords: [{_id: 0, b: 1}],
            localField: "b",
            foreignRecords: docs,
            foreignField: "a",
            idsExpectedToMatch: [[1, 2, 4]]
        });
    })();

    (function testMultipleMatchesOnManyLocal() {
        const localDocs = [
            {_id: 0, a: -1},
            {_id: 1, a: 1},
            {_id: 2, a: 2},
            {_id: 3, a: 3},
            {_id: 4, a: 3},
            {_id: 5, a: 7},
        ];

        const foreignDocs = [
            {_id: 7, b: 0},
            {_id: 8, b: 1},
            {_id: 9, b: 2},
            {_id: 10, b: 2},
            {_id: 11, b: 3},
            {_id: 12, b: 3},
            {_id: 13, b: 3},
            {_id: 14, b: 6},
        ];

        runTest_MultipleLocalForeignRecords({
            testDescription: "Multiple local matches on multiple foreign docs",
            localRecords: localDocs,
            localField: "a",
            foreignRecords: foreignDocs,
            foreignField: "b",
            idsExpectedToMatch: [[], [8], [9, 10], [11, 12, 13], [11, 12, 13], []]
        });
    })();

    return oldSettings;
}

const oldMemSettings =
    assert
        .commandWorked(db.adminCommand({
            getParameter: 1,
            internalQuerySlotBasedExecutionHashLookupApproxMemoryUseInBytesBeforeSpill: 1
        }))
        .internalQuerySlotBasedExecutionHashLookupApproxMemoryUseInBytesBeforeSpill;

(function runAllDiskTest() {
    try {
        // Spill at one byte.
        runHashLookupSpill(1);
    } finally {
        setHashLookupParameters(oldMemSettings);
    }
})();

(function runMixedInMemoryAndDiskTest() {
    try {
        // Spill at 128 bytes.
        runHashLookupSpill(128);
    } finally {
        setHashLookupParameters(oldMemSettings);
    }
})();

(function runMixedAllInMemory() {
    try {
        // Spill at 100 mb.
        runHashLookupSpill(100 * 1024 * 1024);
    } finally {
        setHashLookupParameters(oldMemSettings);
    }
})();
})();
