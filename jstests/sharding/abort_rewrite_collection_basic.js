/**
 * Tests for basic functionality of the abort rewrite collection feature.
 *
 * @tags: [
 *  requires_fcv_83,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

let st = new ShardingTest({mongos: 1, shards: 2});

const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;
let mongos = st.s0;
let shard0 = st.shard0.shardName;
let shard1 = st.shard1.shardName;
let shardKeyMin = -500;
let shardKeyMax = 500;

assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: shard0}));

const coll = mongos.getDB(dbName)[collName];
for (let i = shardKeyMin; i < shardKeyMax; ++i) {
    assert.commandWorked(coll.insert({_id: i}));
}

mongos.getDB(dbName).adminCommand({shardCollection: ns, key: {_id: 1}});

let failpoint = configureFailPoint(st.rs1.getPrimary(), "reshardingPauseRecipientDuringCloning");

const awaitResult = startParallelShell(
    funWithArgs(function (ns) {
        assert.commandFailedWithCode(db.adminCommand({rewriteCollection: ns}), ErrorCodes.ReshardCollectionAborted);
    }, ns),
    st.s.port,
);

failpoint.wait();

// Verify that the provenance field is appended to the currentOp
const filter = {
    type: "op",
    "originatingCommand.reshardCollection": ns,
    "provenance": "rewriteCollection",
};

assert.soon(() => {
    return (
        st.s
            .getDB("admin")
            .aggregate([{$currentOp: {allUsers: true, localOps: false}}, {$match: filter}])
            .toArray().length >= 1
    );
});

assert.commandWorked(mongos.adminCommand({abortRewriteCollection: ns}));

failpoint.off();
awaitResult();

// Confirm that the operation started and was cancelled.
const metrics = st.config0.getDB("admin").serverStatus({}).shardingStatistics.rewriteCollection;

assert.eq(metrics.countStarted, 1);
assert.eq(metrics.countSucceeded, 0);
assert.eq(metrics.countFailed, 0);
assert.eq(metrics.countCanceled, 1);

st.stop();
