/**
 * Tests that resharding can handle the case where there is replication lag on donor shards. That
 * is, during the critical section the resharding oplog fetcher targets the primary node of the
 * donor shard instead of the nearest node.
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {restartServerReplication, stopServerReplication} from "jstests/libs/write_concern_util.js";

const st = new ShardingTest({
    shards: 2,
    rs: {
        nodes: 3,
        // Disallow chaining to force both secondaries to sync from the primary. This disables
        // replication on one of the secondaries, with chaining that would effectively disable
        // replication on both secondaries, causing the test setup to be wrong since writeConcern of
        // w: majority is unsatisfiable.
        settings: {chainingAllowed: false},
    }
});

const dbName = "testDb";
const collName = "testColl";
const ns = dbName + "." + collName;

const testColl = st.s.getCollection(ns);

assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
assert.commandWorked(testColl.insert([{x: -1}, {x: 0}, {x: 1}]));

const configPrimary = st.configRS.getPrimary();
const donorPrimary = st.rs0.getPrimary();

function runMoveCollection(host, ns, toShard) {
    const mongos = new Mongo(host);
    return mongos.adminCommand({moveCollection: ns, toShard});
}

const fp = configureFailPoint(configPrimary, "reshardingPauseCoordinatorBeforeBlockingWrites");

const moveThread = new Thread(runMoveCollection, st.s.host, ns, st.shard1.shardName);
moveThread.start();
fp.wait();

// Pause replication on one of the secondaries. Then, turn on profiling to verify that during the
// critical section the resharding oplog fetcher targets the primary node of the donor shard instead
// of the nearest node. If it still targets a nearest node, the resharding operation would get stuck
// in the critical section.
stopServerReplication(st.rs0.getSecondaries()[1]);
donorPrimary.getDB("local").setProfilingLevel(2);

fp.off();
assert.commandWorked(moveThread.returnData());

const profilerEntries =
    donorPrimary.getDB("local")
        .system.profile
        .find({
            op: "command",
            ns: "local.oplog.rs",
            "command.aggregate": "oplog.rs",
        })
        .toArray()
        .filter(entry => {
            for (let stage of entry.command.pipeline) {
                if (stage.hasOwnProperty("$_internalReshardingIterateTransaction")) {
                    return true;
                }
            }
            return false;
        });
assert.gt(profilerEntries.length, 0);

restartServerReplication(st.rs0.getSecondaries()[1]);
st.stop();
