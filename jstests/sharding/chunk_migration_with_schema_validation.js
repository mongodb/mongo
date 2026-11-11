/**
 * Ensure that schema validation rules applied after a collection has been populated, does not
 * inhibit chunk migration for the data that existed before the rules were applied.
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {CreateShardedCollectionUtil} from "jstests/sharding/libs/create_sharded_collection_util.js";

const st = new ShardingTest({mongos: 1, shards: 2, rs: {nodes: 3}});
const dbName = "test";
const collName = "foo";
const ns = "test.foo";

assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
);
const testColl = st.s.getCollection(ns);

CreateShardedCollectionUtil.shardCollectionWithChunks(testColl, {x: 1}, [
    {min: {x: MinKey}, max: {x: 50}, shard: st.shard0.shardName},
    {min: {x: 50}, max: {x: MaxKey}, shard: st.shard0.shardName},
]);

for (let i = 0; i < 100; i++) {
    assert.commandWorked(testColl.insert({x: i, name: "A"}));
}
assert.eq(100, testColl.find().itcount());

assert.commandWorked(testColl.runCommand({collMod: "foo", validator: {name: {$type: "int"}}}));

let failpoint = configureFailPoint(st.shard1, "migrateThreadHangAtStep4");

const awaitResult = startParallelShell(
    funWithArgs(
        function (ns, toShardName) {
            assert.commandWorked(
                db.adminCommand({
                    moveChunk: ns,
                    find: {x: 50},
                    to: toShardName,
                    _waitForDelete: true,
                }),
            );
        },
        ns,
        st.shard1.shardName,
    ),
    st.s.port,
);

failpoint.wait();

for (let i = 100; i < 200; i++) {
    assert.commandWorked(
        testColl.runCommand({
            insert: collName,
            documents: [{x: i, name: "B"}],
            bypassDocumentValidation: true,
        }),
    );
}

for (let i = 50; i < 75; ++i) {
    assert.commandWorked(testColl.remove({x: i}));
}

// This test runs on multiversion clusters, but only recipient binaries on 9.1 or newer have
// the SERVER-137980 fix to bypass document validation when applying mods in steady state.
const recipientVersion = assert.commandWorked(st.shard1.adminCommand({buildInfo: 1})).version;
const testSteadyState = MongoRunner.compareBinVersions(recipientVersion, "9.1") >= 0;
// Keep writes unblocked after catch-up so the invalid update is applied in steady state.
const steadyStateFailpoint = testSteadyState
    ? configureFailPoint(st.shard0, "moveChunkHangAtStep4")
    : null;
failpoint.off();
if (testSteadyState) {
    steadyStateFailpoint.wait();
    assert.soon(
        () =>
            assert.commandWorked(st.shard1.adminCommand({_recvChunkStatus: 1})).state === "steady",
    );

    assert.commandWorked(
        testColl.runCommand({
            update: collName,
            updates: [{q: {x: 75}, u: {$set: {name: "C"}}}],
            bypassDocumentValidation: true,
        }),
    );

    steadyStateFailpoint.off();
}

awaitResult();

const donor = st.shard0.rs.getPrimary().getDB(dbName);
const recipient = st.shard1.rs.getPrimary().getDB(dbName);
assert.eq(
    50,
    donor.foo.find().itcount(),
    "Number of documents on the donor shard after moveChunk is incorrect.",
);
assert.eq(
    125,
    recipient.foo.find().itcount(),
    "Number of documents on the recipient shard after moveChunk is incorrect.",
);
assert.eq(175, testColl.find().itcount(), "Number of total documents is incorrect");
assert.eq(testSteadyState ? "C" : "A", testColl.findOne({x: 75}).name);

st.stop();
