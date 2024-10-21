import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({
    mongos: [{setParameter: {logComponentVerbosity: tojson({verbosity: 2})}}],
    shard: 2,
    config: 2,
    rs: {
        nodes: 2,
        setParameter: {logComponentVerbosity: tojson({command: 2, executor: 2})},
    },
    other: {
        enableBalancer: false,
    },
    // By default, our test infrastructure sets the election timeout to a very high value (24
    // hours). For this test, we need a shorter election timeout because it relies on nodes running
    // an election when they do not detect an active primary. Therefore, we are setting the
    // electionTimeoutMillis to its default value.
    initiateWithDefaultElectionTimeout: true
});

const dbName = 'test';
const collName = 'testColl';
const ns = dbName + '.' + collName;

const db = st.getDB(dbName);
const coll = db.getCollection(collName);
const mongos = st.s0;

assert.commandWorked(
    mongos.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {x: 1}}));
assert.commandWorked(db.coll.insert({x: 1}));
assert.commandWorked(db.coll.insert({x: -1}));

let s0primary = st.rs0.getPrimary();
let fp = configureFailPoint(s0primary, 'moveChunkHangAtStep1');

assert.commandWorked(mongos.adminCommand({clearLog: 'global'}));

/*
 * After completion of the following `moveRange` command, "shard0" will own documents with the shard
 * key [MinKey, 0), and "shard1" will own documents with the shard key [0, MaxKey).
 */
function moveRangeFunc(ns, rcptShard) {
    assert.commandWorked(db.adminCommand({moveRange: ns, min: {x: 0}, toShard: rcptShard}));
}
const rcptShard = st.shard1.shardName;
const moveRangeThread = startParallelShell(funWithArgs(moveRangeFunc, ns, rcptShard), mongos.port);

// Wait for us to be inside a moveRange command on the config server.
fp.wait();

const stepDownThread = startParallelShell(() => {
    assert.commandWorked(db.adminCommand({"replSetStepDown": 60, force: true}));
}, s0primary.port);

st.rs0.waitForState(s0primary, ReplSetTest.State.SECONDARY);

fp.off();

stepDownThread();
moveRangeThread();

assert(JSON.parse(checkLog.getLogMessage(mongos, "RSM immediate hello check requested")) == null);

st.stop();
