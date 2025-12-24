/**
 * Tests that the multikey metadata set by a CRUD operation in a multi-document transaction is
 * timestamp-consistent among replicas, for both a committed transaction and an aborted transaction.
 *
 * Note that the timestamp consistency cannot be guaranteed after a logical initial sync.
 *
 * TODO (WT-15476): consider removing this test once timestamp consistency is turned on.
 *
 * @tags: [
 *   featureFlagReplicateMultikeynessInTransactions,
 *   requires_replication,
 *   requires_snapshot_read,
 *   uses_transactions,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

function getMultikeyMetadata(node, dbName, collName, indexId, multiPathsKey, ts) {
    return node
        .getDB("admin")
        .aggregate([{$listCatalog: {}}, {$match: {db: dbName, name: collName}}], {
            readConcern: {level: "snapshot", atClusterTime: ts},
        })
        .toArray()[0]["md"]["indexes"][indexId]["multikeyPaths"][multiPathsKey];
}

// Returns the highest timestamp present in the oplog that is less than the given timestamp.
function decrementTimestamp(node, ts) {
    // Query the oplog to find the highest timestamp entry that is lower than the given timestamp.
    const oplogEntry = node
        .getDB("local")
        .oplog.rs.find({ts: {$lt: ts}})
        .sort({ts: -1})
        .limit(1)
        .toArray()[0];

    assert(oplogEntry, "No earlier oplog entry found for timestamp: " + tojson(ts));

    return oplogEntry.ts;
}

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const secondary = rst.getSecondary();
const dbName = "test";
const testDB = primary.getDB(dbName);

const TxnCommitOrAbort = Object.freeze({
    COMMIT: "commit",
    ABORT: "abort",
});

function verifyTimestampConsistency(commitOrAbort, collName) {
    jsTestLog(
        "Verifying multikey timestamp consistency with transaction that will " +
            commitOrAbort +
            " against collection: " +
            collName,
    );

    const testColl = testDB[collName];
    assert.commandWorked(testDB.runCommand({drop: collName}));

    // Create collection and index.
    assert.commandWorked(testColl.createIndex({a: 1}));

    // Start a multi-document transaction.
    const session = testDB.getMongo().startSession();
    const sessionDB = session.getDatabase(dbName);
    const sessionColl = sessionDB[collName];
    session.startTransaction();

    // Insert a document with an array value for field 'a', making the index multikey.
    assert.commandWorked(sessionColl.insert({a: [1, 2]}));

    // Commit the transaction.
    if (commitOrAbort == TxnCommitOrAbort.COMMIT) {
        assert.commandWorked(session.commitTransaction_forTesting());
        jsTestLog("Transaction committed as expected");
    } else {
        assert.commandWorked(session.abortTransaction_forTesting());
        jsTestLog("Transaction aborted as expected");
    }
    session.endSession();

    const latestTs = primary.getDB("local").oplog.rs.find().sort({$natural: -1}).limit(1).toArray()[0].ts;
    jsTestLog("Latest oplog timestamp after transaction " + commitOrAbort + ": " + tojson(latestTs));

    let currentTs = latestTs;
    let timestampsWithMultikeyness = 0;
    const absentMultikeyMetadata = BinData(0, "AA=="); // Base64 encoding of byte [0]

    while (true) {
        jsTestLog("Checking replica set consistency of multikeyness at timestamp: " + tojson(currentTs));
        const primaryMultikeyMetadata = getMultikeyMetadata(primary, dbName, collName, 1, "a", currentTs);
        const secondaryMultikeyMetadata = getMultikeyMetadata(secondary, dbName, collName, 1, "a", currentTs);

        // Verify primary and secondary have identical multikeyness.
        assert.eq(
            primaryMultikeyMetadata,
            secondaryMultikeyMetadata,
            "Primary and secondary multikey paths differ at timestamp " +
                tojson(currentTs) +
                ", iteration: " +
                timestampsWithMultikeyness,
        );

        if (bsonBinaryEqual(primaryMultikeyMetadata, absentMultikeyMetadata)) {
            // If the transaction committed, we expect at least two timestamps with multikeyness:
            // the setMultikeyMetadata timestamp, and the transaction's commit timestamp.
            // If the transaction aborted, we expect only one timestamp with multikeyness:
            // the setMultikeyMetadata timestamp.
            const expectedMinimumMultikeyTimestamps = commitOrAbort == TxnCommitOrAbort.COMMIT ? 2 : 1;
            assert.gte(timestampsWithMultikeyness, expectedMinimumMultikeyTimestamps);
            break;
        }

        timestampsWithMultikeyness++;
        currentTs = decrementTimestamp(primary, currentTs);
    }
}

verifyTimestampConsistency(TxnCommitOrAbort.COMMIT, "coll_commit");
verifyTimestampConsistency(TxnCommitOrAbort.ABORT, "coll_abort");

rst.stopSet();
