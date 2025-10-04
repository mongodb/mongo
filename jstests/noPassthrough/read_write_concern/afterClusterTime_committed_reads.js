// Test that causally consistent majority-committed read-only transactions will wait for the
// majority commit point to move past 'afterClusterTime' before they can commit.
// @tags: [
//   requires_majority_read_concern,
//   uses_transactions,
// ]
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {restartReplicationOnSecondaries, stopReplicationOnSecondaries} from "jstests/libs/write_concern_util.js";

const dbName = "test";
const collName = "coll";

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();

const session = rst.getPrimary().getDB(dbName).getMongo().startSession({causalConsistency: false});
const primaryDB = session.getDatabase(dbName);

let txnNumber = 0;

function testReadConcernLevel(level) {
    // Stop replication.
    stopReplicationOnSecondaries(rst);

    // Perform a write and get its op time.
    const res = assert.commandWorked(primaryDB.runCommand({insert: collName, documents: [{}]}));
    assert(res.hasOwnProperty("opTime"), tojson(res));
    assert(res.opTime.hasOwnProperty("ts"), tojson(res));
    const clusterTime = res.opTime.ts;

    // A majority-committed read-only transaction on the primary after the new cluster time
    // should time out at commit time waiting for the cluster time to be majority committed.
    assert.commandWorked(
        primaryDB.runCommand({
            find: collName,
            txnNumber: NumberLong(++txnNumber),
            startTransaction: true,
            autocommit: false,
            readConcern: {level: level, afterClusterTime: clusterTime},
        }),
    );
    assert.commandFailedWithCode(
        primaryDB.adminCommand({
            commitTransaction: 1,
            txnNumber: NumberLong(txnNumber),
            autocommit: false,
            writeConcern: {w: "majority"},
            maxTimeMS: 1000,
        }),
        ErrorCodes.MaxTimeMSExpired,
    );

    // Restart replication.
    restartReplicationOnSecondaries(rst);

    // A majority-committed read-only transaction on the primary after the new cluster time now
    // succeeds.
    assert.commandWorked(
        primaryDB.runCommand({
            find: collName,
            txnNumber: NumberLong(++txnNumber),
            startTransaction: true,
            autocommit: false,
            readConcern: {level: level, afterClusterTime: clusterTime},
        }),
    );
    assert.commandWorked(
        primaryDB.adminCommand({
            commitTransaction: 1,
            txnNumber: NumberLong(txnNumber),
            autocommit: false,
            writeConcern: {w: "majority"},
        }),
    );
}

testReadConcernLevel("majority");
testReadConcernLevel("snapshot");

rst.stopSet();
