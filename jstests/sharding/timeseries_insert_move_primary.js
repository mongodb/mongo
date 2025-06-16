/**
 * Verifies that timeseries inserts perform a ShardVersion check before inspecting the existance of
 * the buckets collection, with the mongos refreshing if a mismatch is found.
 *
 * @tags: [
 *   requires_timeseries,
 *   uses_parallel_shell,
 * ]
 */

(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");
load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallel_shell_helpers.js");

const st = new ShardingTest({mongos: 1, shards: 3});

Random.setRandomSeed();

const dbName = "test";
const collName = "foo";
const metaField = "mt";
const timeField = "time";
const testDB = st.s.getDB(dbName);
const failpoint = "hangInsertIntoBucketCatalogBeforeCheckingTimeseriesCollection";

if (st.rs0.getPrimary().adminCommand({configureFailPoint: failpoint, mode: "off"}).ok === 0) {
    jsTestLog("Skipping test because the " + failpoint + " fail point is missing");
    st.stop();
    return;
}

function generateRandomTimestamp() {
    const startTime = ISODate("2012-01-01T00:01:00.000Z");
    const maxTime = ISODate("2015-12-31T23:59:59.000Z");
    return new Date(Math.floor(Random.rand() * (maxTime.getTime() - startTime.getTime()) +
                               startTime.getTime()));
}

function runInsertRandomTimeseriesWithIntermittentMovePrimary(orderedInsert) {
    testDB[collName].drop();

    jsTest.log("Move primary");
    assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: st.shard0.shardName}));

    assert.commandWorked(testDB.createCollection(
        collName, {timeseries: {timeField: timeField, metaField: metaField}}));

    let docs = [];
    // Insert 100 documents at random times spanning 3 years (between 2012 and 2015). These dates
    // were chosen arbitrarily.
    for (let i = 0; i < 100; i++) {
        docs.push({[timeField]: generateRandomTimestamp(), [metaField]: "location"});
    }

    let writeFP = configureFailPoint(st.rs0.getPrimary(), failpoint);

    jsTest.log("Begin writes");
    const awaitResult = startParallelShell(
        funWithArgs(function(dbName, collName, docs, orderedInsert) {
            let testDB = db.getSiblingDB(dbName);
            assert.commandWorked(testDB[collName].insertMany(docs, orderedInsert));
        }, dbName, collName, docs, orderedInsert), st.s.port);

    jsTest.log("Wait for failpoint");
    writeFP.wait();

    jsTest.log("Move primary");
    assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: st.shard1.shardName}));

    jsTest.log("Release failpoint and wait for result");
    writeFP.off();
    awaitResult();

    const insertedDocs = testDB[collName].find().toArray();
    assertArrayEq({actual: insertedDocs, expected: docs, fieldsToSkip: ["_id"]});
}

assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

runInsertRandomTimeseriesWithIntermittentMovePrimary({ordered: false});
runInsertRandomTimeseriesWithIntermittentMovePrimary({ordered: true});

st.stop();
})();