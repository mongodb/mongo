// Tests that if a mongoS cursor exceeds the maxTimeMs timeout, the cursors on the shards will be
// cleaned up. Exercises the fix for the bug described in SERVER-62710.
//
// @tags: []

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");  // for 'configureFailPoint()'

function getIdleCursors(conn, collName) {
    return conn.getDB('admin')
        .aggregate([
            {$currentOp: {idleCursors: true}},
            {$match: {$and: [{type: "idleCursor"}, {"cursor.originatingCommand.find": collName}]}}
        ])
        .toArray();
}

function assertNoIdleCursors(conn, collName) {
    const sleepTimeMS = 10 * 1000;
    const retries = 2;
    assert.soon(() => {
        return getIdleCursors(conn, collName).length === 0;
    }, tojson(getIdleCursors(conn, collName)), retries * sleepTimeMS, sleepTimeMS, {
        runHangAnalyzer: false
    });
}

const st = new ShardingTest({shards: 1, mongos: 1, config: 1});

const dbName = "test";
const collName = jsTestName();

const coll = st.s.getCollection(dbName + "." + collName);

assert.commandWorked(coll.insert(Array.from({length: 1000}, _ => ({a: 1}))));

// Perform a query that sleeps after retrieving each document.
// This is guaranteed to exceed the specified maxTimeMS limit.
// The timeout may happen either on mongoS or on shard.
const curs = coll.find({
                     $where: function() {
                         sleep(1);
                         return true;
                     }
                 })
                 .batchSize(2)
                 .maxTimeMS(100);
assert.eq(getIdleCursors(st.shard0, collName).length, 0);
assert.throwsWithCode(() => {
    curs.itcount();
}, ErrorCodes.MaxTimeMSExpired);
assertNoIdleCursors(st.shard0, collName);

// Ensure the timeout happens on mongoS.
const cursTestMongoS = coll.find({}).batchSize(2).maxTimeMS(100);
const fpTestMongoS = configureFailPoint(st.s, "maxTimeAlwaysTimeOut", {}, "alwaysOn");
assert.throwsWithCode(() => {
    cursTestMongoS.itcount();
}, ErrorCodes.MaxTimeMSExpired);
fpTestMongoS.off();
assertNoIdleCursors(st.shard0, collName);

// Ensure the timeout happens on the shard.
const cursTestShard0 = coll.find({}).batchSize(2).maxTimeMS(100);
const fpTestShard0 = configureFailPoint(st.shard0, "maxTimeAlwaysTimeOut", {}, "alwaysOn");
assert.throwsWithCode(() => {
    cursTestShard0.itcount();
}, ErrorCodes.MaxTimeMSExpired);
fpTestShard0.off();
assertNoIdleCursors(st.shard0, collName);

st.stop();
})();
