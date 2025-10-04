/**
 * Checks that a create index targeted to multiple shards will not leave leftover data if
 * interleaved with drop and shard collection.
 * @tags: [
 *   # Cannot step down while the abort failpoint is active.
 *   does_not_support_stepdowns,
 *   # We need to wait for replication after restarting a shard, this requires persistence.
 *   requires_persistence,
 *   # hangCommandBeforeExecution was introduced in 8.2
 *   requires_fcv_82,
 * ]
 */
import {Thread} from "jstests/libs/parallelTester.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {hangCommandBeforeExecution} from "jstests/sharding/libs/failpoint_helpers.js";

// Disable checking for index consistency to ensure that the config server doesn't trigger a
// StaleShardVersion exception on the shards and cause them to refresh their sharding metadata.
const nodeOptions = {
    setParameter: {enableShardedIndexConsistencyCheck: false},
};

const st = new ShardingTest({mongos: 1, shards: 2, other: {configOptions: nodeOptions}});

const kDbName = "db";
const collName = "foo";
const ns = kDbName + "." + collName;
const mongos = st.s0;

jsTestLog("Create collection, shard, split, move chunk to shard1, insert some docs.");
assert.commandWorked(mongos.adminCommand({enableSharding: kDbName, primaryShard: st.shard0.shardName}));
assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {oldKey: 1}}));
assert.commandWorked(mongos.adminCommand({split: ns, middle: {oldKey: 0}}));
assert.commandWorked(mongos.adminCommand({moveChunk: ns, find: {oldKey: 0}, to: st.shard1.shardName}));

let bulk = mongos.getDB(kDbName).getCollection(collName).initializeOrderedBulkOp();
for (let x = -500; x < 500; x++) {
    bulk.insert({oldKey: x, newKey: 500 - x});
}
assert.commandWorked(bulk.execute());

jsTestLog("Enable failpoint to hang createIndexes.");
let hangListIndexes = hangCommandBeforeExecution(st.rs1.getPrimary(), "createIndexes");

let createIndexesThread = new Thread(
    (mongosConnString, dbName, collName) => {
        jsTestLog("Call createIndexes, wait on first abort.");
        let mongos = new Mongo(mongosConnString);
        let mongosColl = mongos.getDB(dbName).getCollection(collName);
        jsTestLog("About to run createIndex command");
        assert.commandWorked(
            mongosColl.runCommand({
                createIndexes: collName,
                indexes: [{key: {yak: 1}, name: "yak_0"}],
            }),
        );
        jsTestLog("Create index command finished.");
    },
    st.s0.host,
    kDbName,
    collName,
);
createIndexesThread.start();

jsTestLog("Wait until createIndexes reach the shard.");
hangListIndexes.wait();

jsTestLog("Drop and create a new collection with the same namespace.");
st.s0.getDB(kDbName).getCollection(collName).drop();
st.s0.adminCommand({shardCollection: ns, key: {_id: 1}});

hangListIndexes.off();
createIndexesThread.join();

jsTestLog("Check that there is only one collection created on the db primary shard.");
let rs1Collections = assert.commandWorked(
    st.rs1
        .getPrimary()
        .getDB(kDbName)
        .runCommand({listCollections: 1, filter: {name: collName}}),
);
assert.eq(0, rs1Collections.cursor.firstBatch.length);
let rs0Collections = assert.commandWorked(
    st.rs0
        .getPrimary()
        .getDB(kDbName)
        .runCommand({listCollections: 1, filter: {name: collName}}),
);
assert.eq(1, rs0Collections.cursor.firstBatch.length);

st.stop();
