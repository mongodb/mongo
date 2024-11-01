/**
 * Tests batched retryable inserts have history copied correctly by resharding and chunk migrations.
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 3});

const dbName = "test";
const collName = "foo";
const coll = st.s.getDB(dbName)[collName];

assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
assert.commandWorked(st.s.adminCommand({shardCollection: coll.getFullName(), key: {x: 1}}));

// Run a retryable bulk insert with at least two documents before resharding to trigger replicating
// them in one applyOps oplog entry.
const lsidBefore = st.s.startSession().getSessionId();
assert.commandWorked(st.s.getDB("test").runCommand({
    insert: "foo",
    documents: [{_id: 0, a: 1, x: -1}, {_id: 1, a: 2, x: -1}],
    lsid: lsidBefore,
    txnNumber: NumberLong(5),
}));

// Start resharding and let it run up to the cloning phase.
let fpHangResharding =
    configureFailPoint(st.configRS.getPrimary(), 'reshardingPauseCoordinatorBeforeBlockingWrites');
let awaitResharding = startParallelShell(funWithArgs(function(ns, toShard) {
                                             assert.commandWorked(db.adminCommand({
                                                 reshardCollection: ns,
                                                 key: {a: 1},
                                                 numInitialChunks: 1,
                                                 shardDistribution: [{shard: toShard}]
                                             }));
                                         }, coll.getFullName(), st.shard1.shardName), st.s.port);
fpHangResharding.wait();

// Run a retryable bulk insert with at least two documents during resharding to trigger replicating
// them in one applyOps oplog entry.
const lsidDuring = st.s.startSession().getSessionId();
assert.commandWorked(st.s.getDB("test").runCommand({
    insert: "foo",
    documents: [{_id: 2, a: 1, x: -1}, {_id: 3, a: 2, x: -1}],
    lsid: lsidDuring,
    txnNumber: NumberLong(1),
}));

fpHangResharding.off();
awaitResharding();
// A retry of a retriable write won't trigger a routing table refresh, so manually force one.
assert.commandWorked(st.s.adminCommand({flushRouterConfig: 1}));

// Verify retrying right after resharding does not double apply.
assert.commandFailedWithCode(st.s.getDB("test").runCommand({
    insert: "foo",
    documents: [{_id: 0, a: 1, x: -1}, {_id: 1, a: 2, x: -1}],
    lsid: lsidBefore,
    txnNumber: NumberLong(5),
}),
                             ErrorCodes.IncompleteTransactionHistory);
assert.commandWorked(st.s.getDB("test").runCommand({
    insert: "foo",
    documents: [{_id: 2, a: 1, x: -1}, {_id: 3, a: 2, x: -1}],
    lsid: lsidDuring,
    txnNumber: NumberLong(1),
}));

// Verify retrying after a subsequent moveChunk also does not double apply.
assert.commandWorked(st.s.adminCommand({split: coll.getFullName(), middle: {a: 0}}));
assert.commandWorked(
    st.s.adminCommand({moveChunk: coll.getFullName(), find: {a: 0}, to: st.shard2.shardName}));

assert.commandFailedWithCode(st.s.getDB("test").runCommand({
    insert: "foo",
    documents: [{_id: 0, a: 1, x: -1}, {_id: 1, a: 2, x: -1}],
    lsid: lsidBefore,
    txnNumber: NumberLong(5),
}),
                             ErrorCodes.IncompleteTransactionHistory);
assert.commandWorked(st.s.getDB("test").runCommand({
    insert: "foo",
    documents: [{_id: 2, a: 1, x: -1}, {_id: 3, a: 2, x: -1}],
    lsid: lsidDuring,
    txnNumber: NumberLong(1)
}));

st.stop();
