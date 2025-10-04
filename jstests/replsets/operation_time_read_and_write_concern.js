/**
 * Validates the operationTime value in the command response depends on the read/writeConcern of the
 * the read/write command that produced it.
 * @tags: [requires_majority_read_concern]
 */
// Skip db hash check because replication is stopped on secondaries.
TestData.skipCheckDBHashes = true;

import {stopReplicationOnSecondaries, restartReplicationOnSecondaries} from "jstests/libs/write_concern_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

let name = "operation_time_read_and_write_concern";

let replTest = new ReplSetTest({name: name, nodes: 3, waitForKeys: true});
replTest.startSet();
replTest.initiate();

// The default WC is majority and stopServerReplication will prevent satisfying any majority writes.
assert.commandWorked(
    replTest
        .getPrimary()
        .adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}),
);
replTest.awaitReplication();

let res;
let testDB = replTest.getPrimary().getDB(name);
let collectionName = "foo";

// readConcern level majority:
// operationTime is the cluster time of the last committed op in the oplog.
jsTestLog("Testing operationTime for readConcern level majority with afterClusterTime.");
let majorityDoc = {_id: 10, x: 1};
let localDoc = {_id: 15, x: 2};

res = assert.commandWorked(
    testDB.runCommand({insert: collectionName, documents: [majorityDoc], writeConcern: {w: "majority"}}),
);
var majorityWriteOperationTime = res.operationTime;

stopReplicationOnSecondaries(replTest, false /* changeReplicaSetDefaultWCToLocal */);

res = assert.commandWorked(testDB.runCommand({insert: collectionName, documents: [localDoc], writeConcern: {w: 1}}));
let localWriteOperationTime = res.operationTime;

assert.gt(localWriteOperationTime, majorityWriteOperationTime);

res = assert.commandWorked(
    testDB.runCommand({
        find: collectionName,
        readConcern: {level: "majority", afterClusterTime: majorityWriteOperationTime},
    }),
);
let majorityReadOperationTime = res.operationTime;

assert.eq(
    res.cursor.firstBatch,
    [majorityDoc],
    "only the committed document, " +
        tojson(majorityDoc) +
        ", should be returned for the majority read with afterClusterTime: " +
        tojson(majorityWriteOperationTime),
);
assert.eq(
    majorityReadOperationTime,
    majorityWriteOperationTime,
    "the operationTime of the majority read, " +
        tojson(majorityReadOperationTime) +
        ", should be the cluster time of the last committed op in the oplog, " +
        tojson(majorityWriteOperationTime),
);

// Validate that after replication, the local write data is now returned by the same query.
restartReplicationOnSecondaries(replTest);
replTest.awaitLastOpCommitted();

res = assert.commandWorked(
    testDB.runCommand({
        find: collectionName,
        sort: {_id: 1}, // So the order of the documents is defined for testing.
        readConcern: {level: "majority", afterClusterTime: majorityWriteOperationTime},
    }),
);
let secondMajorityReadOperationTime = res.operationTime;

assert.eq(
    res.cursor.firstBatch,
    [majorityDoc, localDoc],
    "expected both inserted documents, " +
        tojson([majorityDoc, localDoc]) +
        ", to be returned for the second majority read with afterClusterTime: " +
        tojson(majorityWriteOperationTime),
);
assert.eq(
    secondMajorityReadOperationTime,
    localWriteOperationTime,
    "the operationTime of the second majority read, " +
        tojson(secondMajorityReadOperationTime) +
        ", should be the cluster time of the replicated local write, " +
        tojson(localWriteOperationTime),
);

// readConcern level linearizable is not currently supported.
jsTestLog("Verifying readConcern linearizable with afterClusterTime is not supported.");
res = assert.commandFailedWithCode(
    testDB.runCommand({
        find: collectionName,
        filter: localDoc,
        readConcern: {level: "linearizable", afterClusterTime: majorityReadOperationTime},
    }),
    ErrorCodes.InvalidOptions,
    "linearizable reads with afterClusterTime are not supported and should not be allowed",
);

// writeConcern level majority:
// operationTime is the cluster time of the write if it succeeds, or of the previous successful
// write at the time the write was determined to have failed, or a no-op.
jsTestLog("Testing operationTime for writeConcern level majority.");
let successfulDoc = {_id: 1000, y: 1};
let failedDoc = {_id: 1000, y: 2};

res = assert.commandWorked(
    testDB.runCommand({insert: collectionName, documents: [successfulDoc], writeConcern: {w: "majority"}}),
);
var majorityWriteOperationTime = res.operationTime;

stopReplicationOnSecondaries(replTest, false /* changeReplicaSetDefaultWCToLocal */);

res = testDB.runCommand({
    insert: collectionName,
    documents: [failedDoc],
    writeConcern: {w: "majority", wtimeout: 1000},
});
assert.eq(res.writeErrors[0].code, ErrorCodes.DuplicateKey);
let failedWriteOperationTime = res.operationTime;

assert.eq(
    failedWriteOperationTime,
    majorityWriteOperationTime,
    "the operationTime of the failed majority write, " +
        tojson(failedWriteOperationTime) +
        ", should be the cluster time of the last successful write at the time it failed, " +
        tojson(majorityWriteOperationTime),
);
replTest.stopSet();
