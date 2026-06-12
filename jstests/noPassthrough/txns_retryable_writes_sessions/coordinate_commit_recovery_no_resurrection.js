/**
 * The coordinateCommitTransaction recovery path must not crash if a concurrent
 * startOrContinueTransaction resurrects the participant it just aborted.
 *
 *   1. Start a transaction directly on shard0 (a single find).
 *   2. coordinateCommitTransaction with empty participants -> recovery path. It aborts the local
 *      participant (Block 1) into kAbortedWithoutPrepare, then hangs on the
 *      hangAfterFirstRecoveryCheckout failpoint.
 *   3. Reuse the same (lsid, txnNumber) with startOrContinueTransaction to try to resurrect it.
 *   4. Let recovery proceed (Block 2).
 *
 * @tags: [
 *   requires_sharding,
 *   uses_transactions,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const dbName = "test";
const collName = "coordinate_commit_recovery_no_resurrection";
const ns = dbName + "." + collName;

const st = new ShardingTest({shards: 2});
assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
);
assert.commandWorked(
    st.s.getDB(dbName)[collName].insert({_id: -1}, {writeConcern: {w: "majority"}}),
);
assert.commandWorked(
    st.s.getDB(dbName)[collName].insert({_id: 1}, {writeConcern: {w: "majority"}}),
);
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 0}}));
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {_id: 1}, to: st.shard1.shardName}));
assert.commandWorked(st.rs0.getPrimary().adminCommand({_flushRoutingTableCacheUpdates: ns}));
assert.commandWorked(st.rs1.getPrimary().adminCommand({_flushRoutingTableCacheUpdates: ns}));

const shard0Primary = st.rs0.getPrimary();
const shard0Pid = shard0Primary.pid;
const lsid = {id: UUID()};
const txnNumber = NumberLong(1);

assert.commandWorked(
    shard0Primary.getDB(dbName).runCommand({
        find: collName,
        filter: {_id: -1},
        lsid: lsid,
        txnNumber: txnNumber,
        startTransaction: true,
        autocommit: false,
    }),
);

const failPoint = configureFailPoint(shard0Primary, "hangAfterFirstRecoveryCheckout");

// Recovery completes by finding the txn aborted -> NoSuchTransaction. If shard0 crashed instead,
// this command would get a connection error, so joining the shell below would throw.
const recoveryShell = startParallelShell(
    "assert.commandFailedWithCode(db.adminCommand({" +
        "coordinateCommitTransaction: 1, participants: [], lsid: " +
        tojson(lsid) +
        ", " +
        "txnNumber: NumberLong(1), stmtId: NumberInt(0), autocommit: false}), " +
        "ErrorCodes.NoSuchTransaction);",
    shard0Primary.port,
);

failPoint.wait();

// The sub-router resurrection path: startOrContinueTransaction reusing the aborted (lsid, txnNumber)
// must be refused, so the participant cannot be resurrected while recovery is mid-flight.
assert.commandFailedWithCode(
    shard0Primary.getDB(dbName).runCommand({
        find: collName,
        filter: {_id: -1},
        lsid: lsid,
        txnNumber: txnNumber,
        startOrContinueTransaction: true,
        autocommit: false,
    }),
    ErrorCodes.NoSuchTransaction,
    "startOrContinueTransaction must not restart a participant aborted by recovery",
);

failPoint.off();
recoveryShell();

assert(checkProgram(shard0Pid).alive, "shard0 must not crash on the recovery path");
assert.commandWorked(shard0Primary.adminCommand({hello: 1}));

st.stop();
