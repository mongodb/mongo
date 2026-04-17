/**
 * Tests that resharding aborts an active chunk migration on both the donor and recipient sides when
 * initializing. This prevents a race where both an active migration and resharding are attempting
 * to acquire the critical section.
 *
 * @tags: [
 *   requires_sharding,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({
    shards: 2,
});

const dbName = "test";
const collName = "coll";
const ns = dbName + "." + collName;
const testDB = st.s.getDB(dbName);
const testColl = testDB.getCollection(collName);

jsTest.log("Setup: shard the collection and move all data to shard1.");

assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));

const bulk = testColl.initializeUnorderedBulkOp();
for (let i = 0; i < 10; i++) {
    bulk.insert({x: i, y: i});
}
assert.commandWorked(bulk.execute());

assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {x: 0}, to: st.shard1.shardName, _waitForDelete: true}));

jsTest.log("Configure failpoints.");

const donorStep4Fp = configureFailPoint(st.shard1, "moveChunkHangAtStep4");

const reshardingBeforeCloningFp = configureFailPoint(
    st.configRS.getPrimary(),
    "reshardingPauseCoordinatorBeforeCloning",
);

jsTest.log("Start migration, wait for it to pause at step 4.");

const moveChunkThread = new Thread(
    function (mongosHost, ns, toShard) {
        const mongos = new Mongo(mongosHost);
        return mongos.adminCommand({moveChunk: ns, find: {x: 0}, to: toShard});
    },
    st.s.host,
    ns,
    st.shard0.shardName,
);
moveChunkThread.start();

// Wait for the donor to reach step 4, which is after the cloner deems it's appropriate to enter
// critical section section, but before actually entering it.
donorStep4Fp.wait();

jsTest.log("Start resharding, wait for it to abort the migration.");

const reshardThread = new Thread(
    function (mongosHost, ns) {
        const mongos = new Mongo(mongosHost);
        return mongos.adminCommand({
            reshardCollection: ns,
            key: {y: 1},
            numInitialChunks: 2,
        });
    },
    st.s.host,
    ns,
);
reshardThread.start();

// Wait for resharding to reach the point just before cloning. By this point the coordinator has
// finished initialization (including aborting active migrations).
reshardingBeforeCloningFp.wait();

jsTest.log("Verify: the in-progress migration was aborted.");

moveChunkThread.join();
assert.commandFailed(moveChunkThread.returnData());

jsTest.log("Verify: new chunk migrations cannot start while resharding is active.");

assert.commandFailed(st.s.adminCommand({moveChunk: ns, find: {x: 0}, to: st.shard0.shardName}));

jsTest.log("Resume resharding and verify it succeeds.");

reshardingBeforeCloningFp.off();
reshardThread.join();
assert.commandWorked(reshardThread.returnData());

st.stop();
