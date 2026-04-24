/**
 * Test that $unionWith works when unioning unsharded with sharded collections, and vice versa.
 *
 * @tags: [
 *   assumes_unsharded_collection,
 *   do_not_wrap_aggregations_in_facets,
 *   requires_replication,
 *   requires_sharding,
 * ]
 */

import {arrayEq} from "jstests/aggregation/extras/utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

function getDocsFromCollection(collObj) {
    return collObj.find().toArray();
}
function checkResults(resObj, expectedResult) {
    assert(
        arrayEq(resObj.cursor.firstBatch, expectedResult),
        "Expected:\n" + tojson(expectedResult) + "Got:\n" + tojson(resObj.cursor.firstBatch),
    );
}
const st = new ShardingTest({shards: 2, mongos: 1, config: 1});
const mongos = st.s;
const dbName = jsTestName();
assert.commandWorked(mongos.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
const testDB = mongos.getDB(dbName);
const shardedCollOne = testDB.shardedCollOne;
shardedCollOne.drop();
const shardedCollTwo = testDB.shardedCollTwo;
shardedCollTwo.drop();
const unshardedCollOne = testDB.unshardedCollOne;
unshardedCollOne.drop();
const unshardedCollTwo = testDB.unshardedCollTwo;
unshardedCollTwo.drop();
for (let i = 0; i < 5; i++) {
    assert.commandWorked(shardedCollOne.insert({val: i}));
    assert.commandWorked(shardedCollTwo.insert({val: i * 2}));
    assert.commandWorked(unshardedCollOne.insert({val: i * 3}));
    assert.commandWorked(unshardedCollTwo.insert({val: i * 4}));
}
assert.commandWorked(mongos.adminCommand({shardCollection: shardedCollOne.getFullName(), key: {_id: 1}}));
assert.commandWorked(mongos.adminCommand({shardCollection: shardedCollTwo.getFullName(), key: {_id: 1}}));

// Run each test against both the primary and non-primary shards.
// Make sure the primary is always shard0.
const shardNames = [st.shard0.shardName, st.shard1.shardName];
shardNames.forEach(function (shardName) {
    jsTestLog("Testing with docs on " + shardName);
    testDB.adminCommand({moveChunk: shardedCollOne.getFullName(), find: {_id: 0}, to: shardName});
    testDB.adminCommand({moveChunk: shardedCollTwo.getFullName(), find: {_id: 0}, to: shardName});
    // Test one sharded and one unsharded collection.
    let resSet = getDocsFromCollection(shardedCollOne).concat(getDocsFromCollection(unshardedCollOne));
    let resObj = assert.commandWorked(
        testDB.runCommand({
            aggregate: shardedCollOne.getName(),
            pipeline: [{$unionWith: unshardedCollOne.getName()}],
            cursor: {},
        }),
    );
    checkResults(resObj, resSet);
    resObj = assert.commandWorked(
        testDB.runCommand({
            aggregate: unshardedCollOne.getName(),
            pipeline: [{$unionWith: shardedCollOne.getName()}],
            cursor: {},
        }),
    );
    checkResults(resObj, resSet);

    // Test a union of two sharded collections and one unsharded collection.
    resSet = getDocsFromCollection(shardedCollOne)
        .concat(getDocsFromCollection(unshardedCollOne))
        .concat(getDocsFromCollection(shardedCollTwo));
    resObj = assert.commandWorked(
        testDB.runCommand({
            aggregate: shardedCollOne.getName(),
            pipeline: [
                {
                    $unionWith: {
                        coll: unshardedCollOne.getName(),
                        pipeline: [{$unionWith: shardedCollTwo.getName()}],
                    },
                },
            ],
            cursor: {},
        }),
    );
    checkResults(resObj, resSet);
    // Test a union of two unsharded collections and one sharded collection.
    resSet = getDocsFromCollection(unshardedCollOne)
        .concat(getDocsFromCollection(shardedCollOne))
        .concat(getDocsFromCollection(unshardedCollTwo));
    resObj = assert.commandWorked(
        testDB.runCommand({
            aggregate: unshardedCollOne.getName(),
            pipeline: [
                {
                    $unionWith: {
                        coll: shardedCollOne.getName(),
                        pipeline: [{$unionWith: unshardedCollTwo.getName()}],
                    },
                },
            ],
            cursor: {},
        }),
    );
    checkResults(resObj, resSet);

    // Test a union of two sharded collections when the documents are on different shards.
    jsTestLog("Testing with docs on two different shards");
    testDB.adminCommand({
        moveChunk: shardedCollTwo.getFullName(),
        find: {_id: 0},
        to: st.shard0.shardName == shardName ? st.shard1.shardName : st.shard0.shardName,
    });
    resSet = getDocsFromCollection(shardedCollOne).concat(getDocsFromCollection(shardedCollTwo));
    resObj = assert.commandWorked(
        testDB.runCommand({
            aggregate: shardedCollOne.getName(),
            pipeline: [{$unionWith: shardedCollTwo.getName()}],
            cursor: {},
        }),
    );
    checkResults(resObj, resSet);
    // Test a union of two sharded collections on different shards with an additional unsharded
    // collection.
    resSet = resSet.concat(getDocsFromCollection(unshardedCollOne));
    resObj = assert.commandWorked(
        testDB.runCommand({
            aggregate: shardedCollOne.getName(),
            pipeline: [
                {
                    $unionWith: {
                        coll: unshardedCollOne.getName(),
                        pipeline: [{$unionWith: shardedCollTwo.getName()}],
                    },
                },
            ],
            cursor: {},
        }),
    );
    checkResults(resObj, resSet);
});

// Verify cursorless stages ($collStats, $listCatalog) succeed inside $unionWith sub-pipelines for
// mixed sharded/unsharded topologies.
// The unionWith stage sets the shard version for secondary nss only in case a cursor is required for the local read shortcut.
// Cursorless stages will run versionless, this is fine since they do not require to be strong against migration.
// Those stages must run and pass the version validation when accessing the catalog.
jsTestLog("Testing $unionWith with cursorless sub-pipeline stages for local read shortcut");

// Co-locate shardedCollOne on the primary shard (shard0) so the $unionWith sub-pipeline targets
// the same shard as the outer pipeline. Without this, the sub-pipeline always dispatches remotely
// (to shard0 for unshardedCollOne, or shard1 for shardedCollOne), which creates a new operation
// with proper shard versions and never exercises the local-read path.
assert.commandWorked(
    testDB.adminCommand({moveChunk: shardedCollOne.getFullName(), find: {_id: 0}, to: st.shard0.shardName}),
);

assert.commandWorked(
    testDB.runCommand({
        aggregate: shardedCollOne.getName(),
        pipeline: [
            {$match: {_id: -1}},
            {$unionWith: {coll: unshardedCollOne.getName(), pipeline: [{$collStats: {count: {}, storageStats: {}}}]}},
        ],
        cursor: {},
    }),
);

assert.commandWorked(
    testDB.runCommand({
        aggregate: shardedCollOne.getName(),
        pipeline: [{$unionWith: {coll: unshardedCollOne.getName(), pipeline: [{$listCatalog: {}}]}}],
        cursor: {},
    }),
);

assert.commandWorked(
    testDB.runCommand({
        aggregate: shardedCollOne.getName(),
        pipeline: [
            {$match: {_id: -1}},
            {$unionWith: {coll: unshardedCollOne.getName(), pipeline: [{$collStats: {count: {}, storageStats: {}}}]}},
        ],
        cursor: {},
    }),
);

assert.commandWorked(
    testDB.runCommand({
        aggregate: shardedCollOne.getName(),
        pipeline: [{$unionWith: {coll: unshardedCollOne.getName(), pipeline: [{$listCatalog: {}}]}}],
        cursor: {},
    }),
);

// Change the db primary so the unsharded sub-pipeline namespace (unshardedCollOne) moves to a
// different shard, invalidating cached routing for that namespace. Re-running the same cursorless
// sub-pipeline queries exercises a stale local read which should invalidate the query and retry,
// this time remotely with success.
jsTestLog("Re-running $unionWith with cursorless sub-pipelines after a movePrimary");

assert.commandWorked(testDB.adminCommand({movePrimary: dbName, to: st.shard1.shardName}));
assert.commandWorked(testDB.adminCommand({movePrimary: dbName, to: st.shard0.shardName}));

assert.commandWorked(
    testDB.runCommand({
        aggregate: shardedCollOne.getName(),
        pipeline: [
            {$match: {_id: -1}},
            {$unionWith: {coll: unshardedCollOne.getName(), pipeline: [{$collStats: {count: {}, storageStats: {}}}]}},
        ],
        cursor: {},
    }),
);

assert.commandWorked(
    testDB.runCommand({
        aggregate: shardedCollOne.getName(),
        pipeline: [{$unionWith: {coll: unshardedCollOne.getName(), pipeline: [{$listCatalog: {}}]}}],
        cursor: {},
    }),
);

st.stop();
