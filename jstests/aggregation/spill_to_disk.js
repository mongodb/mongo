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
//   requires_persistence,
// ]
(function() {
'use strict';

load("jstests/aggregation/extras/utils.js");
load('jstests/libs/fixture_helpers.js');            // For 'FixtureHelpers'
load("jstests/libs/sbe_assert_error_override.js");  // Override error-code-checking APIs.
load("jstests/libs/sbe_explain_helpers.js");        // For getSbePlanStages.
load("jstests/libs/sbe_util.js");                   // For checkSBEEnabled.

const coll = db.spill_to_disk;
coll.drop();

// Sets the set parameter named 'paramName' to the given 'memoryLimit' on each primary node in the
// cluster, and returns the old value.
function setMemoryParamHelper(paramName, memoryLimit) {
    const commandResArr = FixtureHelpers.runCommandOnEachPrimary({
        db: db.getSiblingDB("admin"),
        cmdObj: {
            setParameter: 1,
            [paramName]: memoryLimit,
        }
    });
    assert.gt(commandResArr.length, 0, "Setting memory limit on primaries failed");
    const oldMemoryLimit = assert.commandWorked(commandResArr[0]).was;
    return oldMemoryLimit;
}

// Verifies that the given 'groupStats' (an extract from SBE "executionStats" explain output) show
// evidence of spilling to disk.
function assertSpillingOccurredInSbeExplain(groupStats) {
    assert(groupStats);
    assert(groupStats.hasOwnProperty("usedDisk"), groupStats);
    assert(groupStats.usedDisk, groupStats);
    assert.gt(groupStats.numSpills, 0, groupStats);
    assert.gt(groupStats.spilledRecords, 0, groupStats);
    assert.gt(groupStats.spilledDataStorageSize, 0, groupStats);
}

const sharded = FixtureHelpers.isSharded(coll);

const memoryLimitMB = sharded ? 200 : 100;

const isSbeEnabled = checkSBEEnabled(db);

const bigStr = Array(1024 * 1024 + 1).toString();  // 1MB of ','
for (let i = 0; i < memoryLimitMB + 1; i++)
    assert.commandWorked(coll.insert({_id: i, bigStr: i + bigStr, random: Math.random()}));

assert.gt(coll.stats().size, memoryLimitMB * 1024 * 1024);

function test({pipeline, expectedCodes, canSpillToDisk}) {
    // Test that 'allowDiskUse: false' does indeed prevent spilling to disk.
    assert.commandFailedWithCode(
        db.runCommand(
            {aggregate: coll.getName(), pipeline: pipeline, cursor: {}, allowDiskUse: false}),
        expectedCodes);

    // If this command supports spilling to disk, ensure that it will succeed when disk use is
    // allowed.
    const res = db.runCommand(
        {aggregate: coll.getName(), pipeline: pipeline, cursor: {}, allowDiskUse: true});
    if (canSpillToDisk) {
        assert.eq(new DBCommandCursor(coll.getDB(), res).itcount(),
                  coll.count());  // all tests output one doc per input doc

        if (isSbeEnabled) {
            const explain = db.runCommand({
                explain:
                    {aggregate: coll.getName(), pipeline: pipeline, cursor: {}, allowDiskUse: true}
            });
            const hashAggGroups = getSbePlanStages(explain, 'group');
            if (hashAggGroups.length > 0) {
                assert.eq(hashAggGroups.length, 1, explain);
                const hashAggGroup = hashAggGroups[0];
                assertSpillingOccurredInSbeExplain(hashAggGroup);
            }
        }
    } else {
        assert.commandFailedWithCode(res, [ErrorCodes.ExceededMemoryLimit, expectedCodes]);
    }
}

function setHashAggParameters(memoryLimit, atLeast) {
    const memLimitCommandResArr = FixtureHelpers.runCommandOnEachPrimary({
        db: db.getSiblingDB("admin"),
        cmdObj: {
            setParameter: 1,
            internalQuerySlotBasedExecutionHashAggApproxMemoryUseInBytesBeforeSpill: memoryLimit,
        }
    });
    assert.gt(memLimitCommandResArr.length, 0, "Setting memory limit on primaries failed.");
    const oldMemoryLimit = assert.commandWorked(memLimitCommandResArr[0]).was;

    const atLeastCommandResArr = FixtureHelpers.runCommandOnEachPrimary({
        db: db.getSiblingDB("admin"),
        cmdObj: {
            setParameter: 1,
            internalQuerySlotBasedExecutionHashAggMemoryCheckPerAdvanceAtLeast: atLeast,
        }
    });
    assert.gt(atLeastCommandResArr.length, 0, "Setting atLeast limit on primaries failed.");
    const oldAtLeast = assert.commandWorked(atLeastCommandResArr[0]).was;
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

// Test spilling to disk for various accumulators in a $group stage . The data has 5 groups of 10
// documents each. We configure a low memory limit for SBE's hash aggregation stage in order to
// encourage spilling.
const numGroups = 5;
const docsPerGroup = 10;
let counter = 0;
for (let i = 0; i < numGroups; ++i) {
    for (let j = 0; j < docsPerGroup; ++j) {
        const doc = {
            _id: counter++,
            a: i,
            b: 100 * i + j,
            c: 100 * i + j % 5,
            obj: {a: i, b: j},
            random: Math.random()
        };
        assert.commandWorked(coll.insert(doc));
    }
}

function setHashGroupMemoryParameters(memoryLimit) {
    return setMemoryParamHelper(
        "internalQuerySlotBasedExecutionHashAggApproxMemoryUseInBytesBeforeSpill", memoryLimit);
}

// Runs a group query containing the given 'accumulator' after sorting the data by the given
// 'sortInputBy' field. Then verifies that the query results are equal to 'expectedOutput'. If SBE
// is enabled, also runs explain and checks that the execution stats show that spilling occurred.
function testAccumulator({accumulator, sortInputBy, expectedOutput, ignoreArrayOrder = false}) {
    const pipeline =
        [{$sort: {[sortInputBy]: 1}}, {$group: {_id: "$a", acc: accumulator}}, {$sort: {_id: 1}}];
    const results = coll.aggregate(pipeline).toArray();

    if (ignoreArrayOrder) {
        assert(arrayEq(results, expectedOutput));
    } else {
        assert.eq(results, expectedOutput);
    }

    if (isSbeEnabled) {
        const explain = coll.explain("executionStats").aggregate(pipeline);
        const groupStages = getSbePlanStages(explain, "group");
        assert.eq(groupStages.length, 1, groupStages);
        assertSpillingOccurredInSbeExplain(groupStages[0]);
    }
}

function testSpillingForVariousAccumulators() {
    testAccumulator({
        accumulator: {$first: "$b"},
        sortInputBy: "_id",
        expectedOutput: [
            {_id: 0, acc: 0},
            {_id: 1, acc: 100},
            {_id: 2, acc: 200},
            {_id: 3, acc: 300},
            {_id: 4, acc: 400}
        ]

    });

    testAccumulator({
        accumulator: {$last: "$b"},
        sortInputBy: "_id",
        expectedOutput: [
            {_id: 0, acc: 9},
            {_id: 1, acc: 109},
            {_id: 2, acc: 209},
            {_id: 3, acc: 309},
            {_id: 4, acc: 409}
        ]
    });

    testAccumulator({
        accumulator: {$min: "$b"},
        sortInputBy: "random",
        expectedOutput: [
            {_id: 0, acc: 0},
            {_id: 1, acc: 100},
            {_id: 2, acc: 200},
            {_id: 3, acc: 300},
            {_id: 4, acc: 400}
        ]
    });

    testAccumulator({
        accumulator: {$max: "$b"},
        sortInputBy: "random",
        expectedOutput: [
            {_id: 0, acc: 9},
            {_id: 1, acc: 109},
            {_id: 2, acc: 209},
            {_id: 3, acc: 309},
            {_id: 4, acc: 409}
        ]
    });

    testAccumulator({
        accumulator: {$sum: "$b"},
        sortInputBy: "random",
        expectedOutput: [
            {_id: 0, acc: 45},
            {_id: 1, acc: 1045},
            {_id: 2, acc: 2045},
            {_id: 3, acc: 3045},
            {_id: 4, acc: 4045}
        ]
    });

    testAccumulator({
        accumulator: {$avg: "$b"},
        sortInputBy: "random",
        expectedOutput: [
            {_id: 0, acc: 4.5},
            {_id: 1, acc: 104.5},
            {_id: 2, acc: 204.5},
            {_id: 3, acc: 304.5},
            {_id: 4, acc: 404.5}
        ]
    });

    testAccumulator({
        accumulator: {$addToSet: "$c"},
        sortInputBy: "random",
        expectedOutput: [
            {_id: 0, acc: [0, 1, 2, 3, 4]},
            {_id: 1, acc: [100, 101, 102, 103, 104]},
            {_id: 2, acc: [200, 201, 202, 203, 204]},
            {_id: 3, acc: [300, 301, 302, 303, 304]},
            {_id: 4, acc: [400, 401, 402, 403, 404]},
        ],
        // Since the accumulator produces sets, the resulting arrays may be in any order.
        ignoreArrayOrder: true,
    });

    testAccumulator({
        accumulator: {$push: "$c"},
        sortInputBy: "_id",
        expectedOutput: [
            {_id: 0, acc: [0, 1, 2, 3, 4, 0, 1, 2, 3, 4]},
            {_id: 1, acc: [100, 101, 102, 103, 104, 100, 101, 102, 103, 104]},
            {_id: 2, acc: [200, 201, 202, 203, 204, 200, 201, 202, 203, 204]},
            {_id: 3, acc: [300, 301, 302, 303, 304, 300, 301, 302, 303, 304]},
            {_id: 4, acc: [400, 401, 402, 403, 404, 400, 401, 402, 403, 404]},
        ],
    });

    testAccumulator({
        accumulator: {$mergeObjects: "$obj"},
        sortInputBy: "_id",
        expectedOutput: [
            {_id: 0, acc: {a: 0, b: 9}},
            {_id: 1, acc: {a: 1, b: 9}},
            {_id: 2, acc: {a: 2, b: 9}},
            {_id: 3, acc: {a: 3, b: 9}},
            {_id: 4, acc: {a: 4, b: 9}}
        ],
    });
}

(function() {
const kMemLimit = 100;
let oldMemSettings = setHashGroupMemoryParameters(kMemLimit);
try {
    testSpillingForVariousAccumulators();
} finally {
    setHashGroupMemoryParameters(oldMemSettings);
}
})();

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
    return setMemoryParamHelper(
        "internalQuerySlotBasedExecutionHashLookupApproxMemoryUseInBytesBeforeSpill", memoryLimit);
}

/**
 * Executes $lookup with multiple records in the local/foreign collections and checks that the "as"
 * field for it contains documents with ids from `idsExpectedToMatch`. In addition, it checks
 * whether it spills to disk as expected.
 */
function runTest_MultipleLocalForeignRecords({
    testDescription,
    localRecords,
    localField,
    foreignRecords,
    foreignField,
    idsExpectedToMatch,
    spillsToDisk
}) {
    setupCollections(localRecords, foreignRecords, foreignField);
    const pipeline = [{
                  $lookup: {
                                from: foreignColl.getName(),
                                localField: localField,
                                foreignField: foreignField,
                                as: "matched"
                            }}];
    const results = localColl.aggregate(pipeline, {allowDiskUse: true}).toArray();
    const explain = localColl.explain('executionStats').aggregate(pipeline, {allowDiskUse: true});
    // If sharding is enabled, '$lookup' is not pushed down to SBE.
    if (isSbeEnabled && !sharded) {
        const hLookups = getSbePlanStages(explain, 'hash_lookup');
        assert.eq(hLookups.length, 1, explain);
        const hLookup = hLookups[0];
        assert(hLookup, explain);
        assert(hLookup.hasOwnProperty("usedDisk"), hLookup);
        assert.eq(hLookup.usedDisk, spillsToDisk, hLookup);
        if (hLookup.usedDisk) {
            assert.gt(hLookup.spilledRecords, 0, hLookup);
            assert.gt(hLookup.spilledBytesApprox, 0, hLookup);
        }
    }

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

function runHashLookupSpill({memoryLimit, spillsToDisk}) {
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
            idsExpectedToMatch: [[1, 2, 4]],
            spillsToDisk: spillsToDisk
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
            idsExpectedToMatch: [[], [8], [9, 10], [11, 12, 13], [11, 12, 13], []],
            spillsToDisk: spillsToDisk
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
        runHashLookupSpill({memoryLimit: 1, spillsToDisk: true});
    } finally {
        setHashLookupParameters(oldMemSettings);
    }
})();

(function runMixedInMemoryAndDiskTest() {
    try {
        // Spill at 128 bytes.
        runHashLookupSpill({memoryLimit: 128, spillsToDisk: true});
    } finally {
        setHashLookupParameters(oldMemSettings);
    }
})();

(function runMixedAllInMemory() {
    try {
        // Spill at 100 mb.
        runHashLookupSpill({memoryLimit: 100 * 1024 * 1024, spillsToDisk: false});
    } finally {
        setHashLookupParameters(oldMemSettings);
    }
})();
})();
