/**
 * Tests that the decision to engage in the resharding critical section accounts for replication lag
 * on the donor and recipient shards.
 *
 * This test cannot be run in config shard suites since it involves introducing replication lag
 * on all shards, and having replication lag on the config shard can cause various reads against
 * the sharding metadata collection to fail with timeout errors.
 * @tags: [
 *   config_shard_incompatible,
 *   resource_intensive,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {isDisagg} from "jstests/libs/oplog_truncation_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {restartServerReplication, stopServerReplication} from "jstests/libs/write_concern_util.js";

function assertReshardingInApplyingState(mongos, ns) {
    const currentOps = mongos
        .getDB("admin")
        .aggregate([
            {$currentOp: {allUsers: true, localOps: false}},
            {
                $match: {
                    type: "op",
                    "originatingCommand.reshardCollection": ns,
                    recipientState: {$exists: true},
                },
            },
        ])
        .toArray();
    assert.eq(currentOps.length, 1, currentOps);
    assert.eq(currentOps[0].recipientState, "applying", currentOps);
}

const st = new ShardingTest({shards: {rs0: {nodes: 3}, rs1: {nodes: 3}}});

const dbName = "testDb";
const collName = "testColl";
const ns = dbName + "." + collName;

assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
);

const testColl = st.s.getDB(dbName)[collName];
assert.commandWorked(testColl.insert({x: 1}));
assert.commandWorked(st.s.adminCommand({moveCollection: ns, toShard: st.shard1.shardName}));

const configPrimary = st.configRS.getPrimary();
const remainingReshardingOperationTimeThresholdMillis = 500;
const reshardingMaxDelayBetweenRemainingOperationTimeQueriesMillis = 1000;

assert.commandWorked(
    configPrimary.adminCommand({
        setParameter: 1,
        remainingReshardingOperationTimeThresholdMillis,
        reshardingMaxDelayBetweenRemainingOperationTimeQueriesMillis,
    }),
);
let fp = configureFailPoint(configPrimary, "hangBeforeQueryingRecipients");

let moveCollThread = new Thread(
    function (mongosHost, ns, toShard) {
        const conn = new Mongo(mongosHost);
        assert.commandWorked(conn.adminCommand({moveCollection: ns, toShard}));
    },
    st.s.host,
    ns,
    st.shard0.shardName,
);
moveCollThread.start();

fp.wait();

// In disaggregated storage the majority commit point is advanced by the log service rather than
// by secondary acknowledgements, so pausing secondaries does not create majority replication lag.
// Freeze each primary's commit point instead.
const disagg = isDisagg(st.rs0.getPrimary());

function stallMajorityReplication(rs) {
    if (disagg) {
        return configureFailPoint(
            rs.getPrimary(),
            "hangBeforeUpdatingCommittedOpTimeFromLogServer",
        );
    }
    rs.awaitReplication();
    stopServerReplication(rs.getSecondaries());
    return null;
}

// Restores majority replication on 'rs', and returns once the commit point can advance again.
function resumeMajorityReplication(rs, commitFp) {
    if (disagg) {
        commitFp.off();
        return;
    }
    // A single secondary is enough to restore majority replication on a 3-node replica set.
    restartServerReplication(rs.getSecondaries()[0]);
}

jsTest.log(
    "Introduce majority replication lag greater than the threshold for engaging the " +
        "critical section on both the donor and recipient",
);
const rs0CommitFp = stallMajorityReplication(st.rs0);
const rs1CommitFp = stallMajorityReplication(st.rs1);

// Make a write on each primary. In disagg, the commit point update for this write hits the
// failpoint, so the commit point can no longer advance past it.
assert.commandWorked(
    st.rs0
        .getPrimary()
        .adminCommand({appendOplogNote: 1, data: {replLagNoop: 0}, writeConcern: {w: 1}}),
);
assert.commandWorked(
    st.rs1
        .getPrimary()
        .adminCommand({appendOplogNote: 1, data: {replLagNoop: 1}, writeConcern: {w: 1}}),
);
if (disagg) {
    rs0CommitFp.wait();
    rs1CommitFp.wait();
}

sleep(remainingReshardingOperationTimeThresholdMillis + 1);
// These writes are applied locally but cannot be committed, so they are more than the threshold
// ahead of the commit point.
assert.commandWorked(
    st.rs0
        .getPrimary()
        .adminCommand({appendOplogNote: 1, data: {replLagNoop: 2}, writeConcern: {w: 1}}),
);
assert.commandWorked(
    st.rs1
        .getPrimary()
        .adminCommand({appendOplogNote: 1, data: {replLagNoop: 3}, writeConcern: {w: 1}}),
);
fp.off();

jsTest.log(
    "Verify that the critical section cannot be started due to the replication lag on the " +
        "donor and recipient",
);
sleep(reshardingMaxDelayBetweenRemainingOperationTimeQueriesMillis);
assertReshardingInApplyingState(st.s, ns);

jsTest.log(
    "Re-enable majority replication on the recipient and verify that the critical section " +
        "cannot be started due to the replication lag on the donor",
);
resumeMajorityReplication(st.rs0, rs0CommitFp);
sleep(reshardingMaxDelayBetweenRemainingOperationTimeQueriesMillis);
assertReshardingInApplyingState(st.s, ns);

jsTest.log(
    "Re-enable majority replication on the donor and verify that the critical section " +
        "can now be started",
);
resumeMajorityReplication(st.rs1, rs1CommitFp);
moveCollThread.join();

if (!disagg) {
    jsTest.log(
        "Re-enable replication on the remaining secondaries on both the donor and recipient",
    );
    restartServerReplication(st.rs0.getSecondaries());
    restartServerReplication(st.rs1.getSecondaries());
}

st.stop();
