/**
 * Tests $unionWith with various levels of nesting and stages.
 *
 * @tags: [
 *   not_allowed_with_signed_security_token,
 * ]
 */

import "jstests/libs/query/sbe_assert_error_override.js";

import {anyEq} from "jstests/aggregation/extras/utils.js";
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

const testDB = db.getSiblingDB(jsTestName());
const collA = testDB.A;
collA.drop();
const collB = testDB.B;
collB.drop();
const collC = testDB.C;
collC.drop();
const collD = testDB.D;
collD.drop();
const docsPerCollection = 5;
for (let i = 0; i < docsPerCollection; i++) {
    assert.commandWorked(collA.insert({a: i, val: i, groupKey: i}));
    assert.commandWorked(collB.insert({b: i, val: i * 2, groupKey: i}));
    assert.commandWorked(collC.insert({c: i, val: 10 - i, groupKey: i}));
    assert.commandWorked(collD.insert({d: i, val: i * 3, groupKey: i}));
}

function checkResults(resObj, expectedResult) {
    assert.commandWorked(resObj);
    assert(
        anyEq(resObj.cursor.firstBatch, expectedResult),
        "Expected:\n" + tojson(expectedResult) + "Got:\n" + tojson(resObj.cursor.firstBatch),
    );
}

function getDocsFromCollection(collObj, proj = null) {
    if (proj == null) {
        return collObj.find().toArray();
    } else {
        return collObj.aggregate({"$addFields": proj}).toArray();
    }
}
// Test a two collection union.
let resSet = getDocsFromCollection(collA).concat(getDocsFromCollection(collB));
checkResults(
    testDB.runCommand({aggregate: collA.getName(), pipeline: [{$unionWith: collB.getName()}], cursor: {}}),
    resSet,
);
// Test a sequential four collection union.
resSet = getDocsFromCollection(collA).concat(
    getDocsFromCollection(collB),
    getDocsFromCollection(collC),
    getDocsFromCollection(collD),
);
checkResults(
    testDB.runCommand({
        aggregate: collA.getName(),
        pipeline: [{$unionWith: collB.getName()}, {$unionWith: collC.getName()}, {$unionWith: collD.getName()}],
        cursor: {},
    }),
    resSet,
);
// Test a nested four collection union.
// resSet should be the same.
checkResults(
    testDB.runCommand({
        aggregate: collA.getName(),
        pipeline: [
            {
                $unionWith: {
                    coll: collB.getName(),
                    pipeline: [{$unionWith: {coll: collC.getName(), pipeline: [{$unionWith: collD.getName()}]}}],
                },
            },
        ],
        cursor: {},
    }),
    resSet,
);
// Test that a sub-pipeline is applied to the correct documents.
resSet = getDocsFromCollection(collA).concat(getDocsFromCollection(collB, {x: 3}));
checkResults(
    testDB.runCommand({
        aggregate: collA.getName(),
        pipeline: [{$unionWith: {coll: collB.getName(), pipeline: [{"$addFields": {x: 3}}]}}],
        cursor: {},
    }),
    resSet,
);
// Test that for multiple nested unions sub-pipelines are applied to the correct documents.
resSet = getDocsFromCollection(collA).concat(
    getDocsFromCollection(collB, {x: 3}),
    getDocsFromCollection(collC, {x: 3, y: 4}),
    getDocsFromCollection(collD, {x: 3, y: 4, z: 5}),
);
checkResults(
    testDB.runCommand({
        aggregate: collA.getName(),
        pipeline: [
            {
                $unionWith: {
                    coll: collB.getName(),
                    pipeline: [
                        {
                            $unionWith: {
                                coll: collC.getName(),
                                pipeline: [
                                    {
                                        $unionWith: {coll: collD.getName(), pipeline: [{"$addFields": {z: 5}}]},
                                    },
                                    {"$addFields": {y: 4}},
                                ],
                            },
                        },
                        {"$addFields": {x: 3}},
                    ],
                },
            },
        ],
        cursor: {},
    }),
    resSet,
);
resSet = getDocsFromCollection(collA).concat(
    getDocsFromCollection(collB, {x: 3}),
    getDocsFromCollection(collC, {x: 3, y: 4}),
    getDocsFromCollection(collD, {x: 3, z: 5}),
);
checkResults(
    testDB.runCommand({
        aggregate: collA.getName(),
        pipeline: [
            {
                $unionWith: {
                    coll: collB.getName(),
                    pipeline: [
                        {$unionWith: {coll: collC.getName(), pipeline: [{"$addFields": {y: 4}}]}},
                        {$unionWith: {coll: collD.getName(), pipeline: [{"$addFields": {z: 5}}]}},
                        {"$addFields": {x: 3}},
                    ],
                },
            },
        ],
        cursor: {},
    }),
    resSet,
);

function setHashAggParameters(memoryLimit, atLeast) {
    FixtureHelpers.runCommandOnEachPrimary({
        db: testDB.getSiblingDB("admin"),
        cmdObj: {
            setParameter: 1,
            "internalDocumentSourceGroupMaxMemoryBytes": memoryLimit,
        },
    });

    FixtureHelpers.runCommandOnEachPrimary({
        db: testDB.getSiblingDB("admin"),
        cmdObj: {
            setParameter: 1,
            "internalQuerySlotBasedExecutionHashAggApproxMemoryUseInBytesBeforeSpill": memoryLimit,
        },
    });

    FixtureHelpers.runCommandOnEachPrimary({
        db: testDB.getSiblingDB("admin"),
        cmdObj: {
            setParameter: 1,
            "internalQuerySlotBasedExecutionHashAggMemoryCheckPerAdvanceAtLeast": atLeast,
        },
    });
}

function testWithHashAggMemoryLimit(command, resSet, memoryLimit, expectedError) {
    try {
        setHashAggParameters(memoryLimit, 1 /*atLeast*/);
        if (!expectedError) {
            checkResults(testDB.runCommand(command), resSet);
        } else {
            assert.commandFailedWithCode(testDB.runCommand(command), expectedError);
        }
    } finally {
        setHashAggParameters(100 * 1024 * 1024 /* memoryLimit */, 1024 /* atLeast */);
    }
}

resSet = [
    {_id: 0, sum: 0},
    {_id: 1, sum: 3},
    {_id: 2, sum: 6},
    {_id: 3, sum: 9},
    {_id: 4, sum: 12},
];
testWithHashAggMemoryLimit(
    {
        aggregate: collA.getName(),
        pipeline: [{$unionWith: collB.getName()}, {"$group": {_id: "$groupKey", sum: {$sum: "$val"}}}],
        cursor: {},
        // we allow disk use to test the case where the pipeline must run on a shard in the sharded
        // passthrough.
        allowDiskUse: true,
    },
    resSet,
    1 /* 1MB memory limit */,
);

// Test a $group that sums in both the inner and outer pipeline.
resSet = [
    {_id: 0, sum: 0},
    {_id: 1, sum: 21},
    {_id: 2, sum: 2},
    {_id: 3, sum: 3},
    {_id: 4, sum: 4},
];
testWithHashAggMemoryLimit(
    {
        aggregate: collA.getName(),
        pipeline: [
            {
                $unionWith: {
                    coll: collB.getName(),
                    pipeline: [{"$group": {_id: "$groupKey", val: {$sum: "$val"}}}, {"$addFields": {groupKey: 1}}],
                },
            },
            {"$group": {_id: "$groupKey", sum: {$sum: "$val"}}},
        ],
        allowDiskUse: true,
        cursor: {},
    },
    resSet,
    1 /* 1MB memory limit */,
);

// Test a $group in the inner pipeline.
resSet = getDocsFromCollection(collA).concat([
    {_id: 0, sum: 0},
    {_id: 1, sum: 2},
    {_id: 2, sum: 4},
    {_id: 3, sum: 6},
    {_id: 4, sum: 8},
]);
testWithHashAggMemoryLimit(
    {
        aggregate: collA.getName(),
        pipeline: [
            {
                $unionWith: {
                    coll: collB.getName(),
                    pipeline: [{"$group": {_id: "$groupKey", sum: {$sum: "$val"}}}],
                },
            },
        ],
        allowDiskUse: true,
        cursor: {},
    },
    resSet,
    1 /* 1MB memory limit */,
);

// Test that a $group within a $unionWith sub-pipeline correctly fails if it needs to spill but
// 'allowDiskUse' is false.
testWithHashAggMemoryLimit(
    {
        aggregate: collA.getName(),
        pipeline: [
            {
                $unionWith: {
                    coll: collB.getName(),
                    pipeline: [{"$group": {_id: "$groupKey", val: {$min: "$val"}}}, {"$addFields": {groupKey: 1}}],
                },
            },
        ],
        allowDiskUse: false,
        cursor: {},
    },
    {} /* resSet */,
    1 /* 1MB memory limit */,
    ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed,
);

// Test with $limit and sort in a sub-pipeline.
const setBResult = collB.find().sort({b: 1}).limit(2).toArray();
resSet = getDocsFromCollection(collA).concat(setBResult);
checkResults(
    testDB.runCommand({
        aggregate: collA.getName(),
        pipeline: [{$unionWith: {coll: collB.getName(), pipeline: [{"$sort": {b: 1}}, {"$limit": 2}]}}],
        cursor: {},
    }),
    resSet,
);

// Test that we get the correct number of documents when using a getmore.
resSet = getDocsFromCollection(collA).concat(getDocsFromCollection(collD)).length;
let fullResults = collA
    .aggregate([{$unionWith: collD.getName()}], {cursor: {batchSize: Math.floor(docsPerCollection / 2)}})
    .itcount();
assert.eq(resSet, fullResults, "Expected: " + resSet + " Got: " + fullResults.length + " from " + tojson(fullResults));

// Test with a sub-pipeline and a getMore before the initial collection is exhausted.
resSet = getDocsFromCollection(collA).concat(getDocsFromCollection(collD, {x: 3})).length;
fullResults = collA
    .aggregate([{$unionWith: {coll: collD.getName(), pipeline: [{"$addFields": {x: 3}}]}}], {
        cursor: {batchSize: Math.floor(docsPerCollection / 2)},
    })
    .itcount();
assert.eq(resSet, fullResults, "Expected: " + resSet + " Got: " + fullResults.length + " from " + tojson(fullResults));

// Test when a getMore occurs after the initial collection is exhausted. resSet remains the same as
// only batchSize has changed.
fullResults = collA
    .aggregate([{$unionWith: {coll: collD.getName(), pipeline: [{"$addFields": {x: 3}}]}}], {
        cursor: {batchSize: docsPerCollection + 1},
    })
    .itcount();
assert.eq(resSet, fullResults, "Expected: " + resSet + " Got: " + fullResults.length + " from " + tojson(fullResults));

// Test that $unionWith on a non-existent collection succeeds.
resSet = getDocsFromCollection(collA);
checkResults(
    testDB.runCommand({
        aggregate: collA.getName(),
        pipeline: [{$unionWith: "nonExistentCollectionAlpha"}],
        cursor: {},
    }),
    resSet,
);
checkResults(
    testDB.runCommand({
        aggregate: collA.getName(),
        pipeline: [{$unionWith: {coll: "nonExistentCollectionBeta", pipeline: [{"$addFields": {ted: 5}}]}}],
        cursor: {},
    }),
    resSet,
);
checkResults(
    testDB.runCommand({
        aggregate: "nonExistentCollectionCharlie",
        pipeline: [{$unionWith: collA.getName()}],
        cursor: {},
    }),
    resSet,
);

// Test that $unionWith on an empty collection succeeds.
assertDropAndRecreateCollection(testDB, "emptyCollection");
checkResults(
    testDB.runCommand({aggregate: "emptyCollection", pipeline: [{$unionWith: collA.getName()}], cursor: {}}),
    resSet,
);
checkResults(
    testDB.runCommand({
        aggregate: collA.getName(),
        pipeline: [{$unionWith: {coll: "emptyCollection", pipeline: [{"$addFields": {ted: 5}}]}}],
        cursor: {},
    }),
    resSet,
);
checkResults(
    testDB.runCommand({aggregate: "emptyCollection", pipeline: [{$unionWith: collA.getName()}], cursor: {}}),
    resSet,
);
