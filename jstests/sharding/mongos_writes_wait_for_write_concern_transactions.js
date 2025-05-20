/**
 * Tests that commands that accept write concern correctly return write concern errors when run
 * through mongos.
 *
 * @tags: [
 * assumes_balancer_off,
 * does_not_support_stepdowns,
 * multiversion_incompatible,
 * uses_transactions,
 * ]
 *
 */

import {precmdShardKey} from "jstests/libs/write_concern_all_commands.js";
import {assertWriteConcernError} from "jstests/libs/write_concern_util.js";

var st = new ShardingTest({
    mongos: 1,
    shards: 2,
    rs: {nodes: 3},
    other: {rsOptions: {settings: {electionTimeoutMillis: ReplSetTest.kForeverMillis}}},
    configReplSetTestOptions: {settings: {electionTimeoutMillis: ReplSetTest.kForeverMillis}}
});

assert.commandWorked(
    st.s.adminCommand({setDefaultRWConcern: 1, defaultReadConcern: {"level": "local"}}));

jsTest.log("Testing commit and abort in cross shard transactions.");
// Test that commit and abort transaction return write concern errors as expected in cross shard
// transctions
let dbName = "test";
let collName = "foo";

precmdShardKey("x", st.s, st, dbName, collName);
let lsid = UUID();

// Stop one of the secondaries on shard0 so that w:3 will fail.
let secondary = st.rs0.getSecondary();
st.rs0.stop(secondary);

// *** 2PC tests ***

// 1. Test that commitTransaction returns a WCE when there are two writes shards
assert.commandWorked(st.s.getDB(dbName).runCommand({
    insert: collName,
    documents: [{x: -10}, {x: 10}],
    lsid: {id: lsid},
    stmtIds: [NumberInt(0), NumberInt(1)],
    txnNumber: NumberLong(0),
    startTransaction: true,
    autocommit: false
}));
let commitRes = st.s.adminCommand({
    commitTransaction: 1,
    lsid: {id: lsid},
    txnNumber: NumberLong(0),
    autocommit: false,
    writeConcern: {w: 3, wtimeout: 1000}
});
assert.commandWorkedIgnoringWriteConcernErrors(commitRes);
assertWriteConcernError(commitRes);
assert.eq(st.s.getDB(dbName).foo.find({x: -10}).itcount(), 1);

// 2. Test that abortTransaction returns a WCE when there are two writes shards and one is prepared.
// abortTransaction will only wait for write concern if it needs to do a no-op write, so we must
// put the shard in prepared state to force the abort path to write.
assert.commandWorked(st.s.getDB(dbName).runCommand({
    insert: collName,
    documents: [{x: -5}, {x: 5}],
    lsid: {id: lsid},
    stmtIds: [NumberInt(0), NumberInt(1)],
    txnNumber: NumberLong(1),
    startTransaction: true,
    autocommit: false
}));
assert.commandWorked(st.shard0.getDB(dbName).adminCommand(
    {prepareTransaction: 1, lsid: {id: lsid}, txnNumber: NumberLong(1), autocommit: false}));

let abortRes = st.s.adminCommand({
    abortTransaction: 1,
    lsid: {id: lsid},
    txnNumber: NumberLong(1),
    autocommit: false,
    writeConcern: {w: 3, wtimeout: 1000}
});
assert.commandWorkedIgnoringWriteConcernErrors(abortRes);
assertWriteConcernError(abortRes);
assert.eq(st.s.getDB(dbName).foo.find({x: -5}).itcount(), 0);

// 3. Test that abortTransctions returns a WCE if we attempt to abort after a commit has succeeded
// but failed write concern
assert.commandWorked(st.s.getDB(dbName).runCommand({
    insert: collName,
    documents: [{x: -1}, {x: 1}],
    lsid: {id: lsid},
    stmtIds: [NumberInt(0), NumberInt(1)],
    txnNumber: NumberLong(2),
    startTransaction: true,
    autocommit: false
}));

commitRes = st.s.adminCommand({
    commitTransaction: 1,
    lsid: {id: lsid},
    txnNumber: NumberLong(2),
    autocommit: false,
    writeConcern: {w: 3, wtimeout: 1000}
});
assert.commandWorkedIgnoringWriteConcernErrors(commitRes);
assertWriteConcernError(commitRes);
assert.eq(st.s.getDB(dbName).foo.find({x: 1}).itcount(), 1);

abortRes = st.s.adminCommand({
    abortTransaction: 1,
    lsid: {id: lsid},
    txnNumber: NumberLong(2),
    autocommit: false,
    writeConcern: {w: 3, wtimeout: 1000}
});
assert.commandFailedWithCode(abortRes, ErrorCodes.TransactionCommitted);
assertWriteConcernError(abortRes);

// *** 1 shard write commit optimization path tests ***

// 1. Test that commitTransaction returns a WCE when there is 1 write shard, and 1 read shard, and
// the write shard fails a WCE
st.rs1.awaitReplication();

assert.commandWorked(st.s.getDB(dbName).runCommand({
    insert: collName,
    documents: [{x: -20}],
    lsid: {id: lsid},
    stmtIds: [NumberInt(0)],
    txnNumber: NumberLong(3),
    startTransaction: true,
    autocommit: false
}));
assert.commandWorked(st.s.getDB(dbName).runCommand({
    find: collName,
    filter: {x: 1},
    lsid: {id: lsid},
    stmtId: NumberInt(1),
    txnNumber: NumberLong(3),
    autocommit: false
}));
commitRes = st.s.adminCommand({
    commitTransaction: 1,
    lsid: {id: lsid},
    txnNumber: NumberLong(3),
    autocommit: false,
    writeConcern: {w: 3, wtimeout: 1000}
});
assert.commandWorkedIgnoringWriteConcernErrors(commitRes);
assertWriteConcernError(commitRes);
assert.eq(st.s.getDB(dbName).foo.find({x: -20}).itcount(), 1);

// 2. Test that commitTransaction returns a WCE when there is 1 write shard, and 1 read shard, and
// the read shard fails a WCE
st.rs1.awaitReplication();

assert.commandWorked(st.s.getDB(dbName).runCommand({
    insert: collName,
    documents: [{x: 20}],
    lsid: {id: lsid},
    stmtIds: [NumberInt(0)],
    txnNumber: NumberLong(4),
    startTransaction: true,
    autocommit: false
}));
assert.commandWorked(st.s.getDB(dbName).runCommand({
    find: collName,
    filter: {x: -1},
    lsid: {id: lsid},
    stmtId: NumberInt(1),
    txnNumber: NumberLong(4),
    autocommit: false
}));
commitRes = st.s.adminCommand({
    commitTransaction: 1,
    lsid: {id: lsid},
    txnNumber: NumberLong(4),
    autocommit: false,
    writeConcern: {w: 3, wtimeout: 1000}
});
assert.commandFailedWithCode(commitRes, ErrorCodes.WriteConcernFailed);
assert.eq(st.s.getDB(dbName).foo.find({x: 20}).itcount(), 0);

// Restart the secondary
st.rs0.restart(secondary);

st.stop();
