/**
 * Tests that the multikey metadata set by a CRUD operation in a multi-document transaction is
 * timestamp-consistent among replicas, for both a committed transaction and an aborted transaction.
 *
 * Covers both regular indexes (multikey paths tracked in the catalog) and wildcard indexes
 * (multikey paths tracked as metadata key entries in the index itself).
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

const IndexType = Object.freeze({
    REGULAR: "regular",
    WILDCARD: "wildcard",
});

/**
 * Returns multikey metadata for the given index at the given timestamp, or null if the
 * collection/index does not yet exist at that snapshot.
 *
 * For regular indexes, reads the multikeyPaths bitset from the catalog ($listCatalog).
 * For wildcard indexes, reads the multiKeyPaths from the query planner (explain with atClusterTime),
 * which reflects the actual metadata key entries in the index.
 */
function getMultikeyMetadata(node, indexType, dbName, collName, indexId, multiPathsKey, wildcardHint, ts) {
    if (indexType == IndexType.REGULAR) {
        const catalog = node
            .getDB("admin")
            .aggregate([{$listCatalog: {}}, {$match: {db: dbName, name: collName}}], {
                readConcern: {level: "snapshot", atClusterTime: ts},
            })
            .toArray();
        // Collection does not exist at this snapshot (same-txn create-then-insert case walks
        // timestamps earlier than the collection's creation).
        if (catalog.length === 0) {
            return null;
        }
        const idx = catalog[0]["md"]["indexes"][indexId];
        if (!idx) {
            return null;
        }
        return idx["multikeyPaths"][multiPathsKey];
    } else {
        const explain = node.getDB(dbName).runCommand({
            explain: {find: collName, filter: {[multiPathsKey]: 1}, hint: wildcardHint},
            readConcern: {level: "snapshot", atClusterTime: ts},
            verbosity: "queryPlanner",
        });
        // If the collection does not yet exist at this snapshot, explain returns an error.
        if (!explain.ok) {
            return null;
        }
        // Walks the explain tree to find the wildcard IXSCAN. Handles both engines:
        //   - Classic: winningPlan.{stage:..., inputStage:{...}}
        //   - SBE: winningPlan.queryPlan.{stage:..., inputStage:{...}}
        // SBE wraps the QuerySolutionNode tree under a 'queryPlan' field; recurse into it.
        function findWildcardIxscan(plan) {
            if (!plan) {
                return null;
            }
            if (plan.stage === "IXSCAN" && plan.keyPattern && plan.keyPattern["$_path"]) {
                return plan;
            }
            if (plan.queryPlan) {
                const r = findWildcardIxscan(plan.queryPlan);
                if (r) {
                    return r;
                }
            }
            if (plan.inputStage) {
                return findWildcardIxscan(plan.inputStage);
            }
            if (plan.inputStages) {
                for (const s of plan.inputStages) {
                    const result = findWildcardIxscan(s);
                    if (result) {
                        return result;
                    }
                }
            }
            return null;
        }
        const ixscan = findWildcardIxscan(explain.queryPlanner.winningPlan);
        return ixscan?.multiKeyPaths?.[multiPathsKey] ?? [];
    }
}

/**
 * Returns true if the multikey metadata indicates "not multikey" for the given index type, or the
 * collection/index does not exist yet at this snapshot.
 */
function absentMultikeyMetadata(multikeyMetadata, indexType) {
    if (multikeyMetadata === null) {
        // Collection/index not yet created at this snapshot.
        return true;
    }
    if (indexType == IndexType.REGULAR) {
        return bsonBinaryEqual(multikeyMetadata, BinData(0, "AA==")); // byte [0]
    } else {
        return multikeyMetadata.length === 0;
    }
}

/**
 * Returns the highest timestamp present in the oplog that is less than the given timestamp.
 */
function decrementTimestamp(node, ts) {
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

/**
 * Walks the oplog backwards from the latest timestamp. At each timestamp, fetches multikey
 * metadata on both primary and secondary and asserts they are identical. Stops when it finds a
 * timestamp at which multikey metadata is absent (i.e. before it was first recorded). Asserts that
 * at least `expectedMinTimestamps` timestamps had multikey metadata.
 */
function walkAndAssertReplicaMultikeyConsistency({
    indexType,
    collName,
    indexId,
    multiPathsKey,
    wildcardHint,
    expectedMinTimestamps,
    context,
}) {
    // Ensure primaries have caught up before walking.
    rst.awaitReplication();

    const latestTs = primary.getDB("local").oplog.rs.find().sort({$natural: -1}).limit(1).toArray()[0].ts;
    jsTestLog("Latest oplog timestamp for " + context + ": " + tojson(latestTs));

    let currentTs = latestTs;
    let timestampsWithMultikeyness = 0;

    while (true) {
        jsTestLog(
            "Checking replica set multikey consistency at timestamp: " + tojson(currentTs) + " (" + context + ")",
        );
        const primaryMultikeyMetadata = getMultikeyMetadata(
            primary,
            indexType,
            dbName,
            collName,
            indexId,
            multiPathsKey,
            wildcardHint,
            currentTs,
        );
        const secondaryMultikeyMetadata = getMultikeyMetadata(
            secondary,
            indexType,
            dbName,
            collName,
            indexId,
            multiPathsKey,
            wildcardHint,
            currentTs,
        );

        assert.eq(
            primaryMultikeyMetadata,
            secondaryMultikeyMetadata,
            "Primary and secondary multikey metadata differ at timestamp " +
                tojson(currentTs) +
                " (" +
                context +
                "), iteration: " +
                timestampsWithMultikeyness,
        );

        if (absentMultikeyMetadata(primaryMultikeyMetadata, indexType)) {
            assert.gte(
                timestampsWithMultikeyness,
                expectedMinTimestamps,
                "Expected at least " + expectedMinTimestamps + " timestamp(s) with multikey metadata for " + context,
            );
            break;
        }

        timestampsWithMultikeyness++;
        currentTs = decrementTimestamp(primary, currentTs);
    }
}

/**
 * Verifies timestamp-consistency of multikey metadata between primary and secondary after a
 * multi-document transaction that inserts into a pre-existing indexed collection.
 *
 * Expected minimum timestamps:
 *  - Txn commit: 2 (setMultikeyMetadata entry + commit entry)
 *  - Txn abort:  1 (setMultikeyMetadata entry committed by the side txn survives abort)
 */
function verifyTimestampConsistency(commitOrAbort, indexType, collName) {
    jsTestLog(
        "Verifying multikey timestamp consistency for " +
            indexType +
            " index against collection '" +
            collName +
            "' with transaction that will " +
            commitOrAbort,
    );

    const testColl = testDB[collName];
    assert.commandWorked(testDB.runCommand({drop: collName}));

    if (indexType == IndexType.REGULAR) {
        assert.commandWorked(testColl.createIndex({a: 1}));
    } else {
        assert.commandWorked(testColl.createIndex({"$**": 1}));
    }

    const session = testDB.getMongo().startSession();
    const sessionDB = session.getDatabase(dbName);
    const sessionColl = sessionDB[collName];
    session.startTransaction();

    assert.commandWorked(sessionColl.insert({a: [1, 2]}));

    if (commitOrAbort == TxnCommitOrAbort.COMMIT) {
        assert.commandWorked(session.commitTransaction_forTesting());
        jsTestLog("Transaction committed as expected");
    } else {
        assert.commandWorked(session.abortTransaction_forTesting());
        jsTestLog("Transaction aborted as expected");
    }
    session.endSession();

    // Commit: both the setMultikeyMetadata side-txn oplog entry and the parent commit's insert
    //   emit multikey state updates — 2 distinct timestamps show multikey metadata.
    // Abort: only the setMultikeyMetadata side-txn oplog entry survives the aborted parent —
    //   1 timestamp shows multikey metadata.
    const expectedMinTimestamps = commitOrAbort == TxnCommitOrAbort.COMMIT ? 2 : 1;

    walkAndAssertReplicaMultikeyConsistency({
        indexType,
        collName,
        indexId: 1, // index 0 is _id, index 1 is the user index.
        multiPathsKey: "a",
        wildcardHint: {"$**": 1},
        expectedMinTimestamps,
        context: indexType + "/" + commitOrAbort + "/" + collName,
    });
}

verifyTimestampConsistency(TxnCommitOrAbort.COMMIT, IndexType.REGULAR, "coll_regular_commit");
verifyTimestampConsistency(TxnCommitOrAbort.ABORT, IndexType.REGULAR, "coll_regular_abort");
verifyTimestampConsistency(TxnCommitOrAbort.COMMIT, IndexType.WILDCARD, "coll_wildcard_commit");
verifyTimestampConsistency(TxnCommitOrAbort.ABORT, IndexType.WILDCARD, "coll_wildcard_abort");

/**
 * Same as `verifyTimestampConsistency`, but creates the collection + index inside the same
 * transaction as the multikey-triggering insert. Exercises the path where the side transaction
 * cannot see the index (it was created in the parent transaction), so metadata keys are written in
 * the parent transaction instead.
 *
 * Reuses the shared walker to assert primary/secondary multikey metadata consistency at every
 * oplog timestamp. For this path, multikey metadata becomes visible starting at the commit
 * timestamp, so the minimum expected count is 1.
 */
function verifyIndexCreatedInSameTransaction(indexType, collName) {
    jsTestLog(
        "Verifying multikey timestamp consistency for " +
            indexType +
            " index created in same transaction against collection '" +
            collName +
            "'",
    );

    assert.commandWorked(testDB.runCommand({drop: collName}));

    const session = testDB.getMongo().startSession();
    const sessionDB = session.getDatabase(dbName);
    session.startTransaction();

    // Create collection and index inside the transaction.
    assert.commandWorked(sessionDB.createCollection(collName));
    if (indexType == IndexType.REGULAR) {
        assert.commandWorked(sessionDB[collName].createIndex({a: 1}));
    } else {
        assert.commandWorked(sessionDB[collName].createIndex({"$**": 1}));
    }

    // Insert a document that makes the index multikey.
    assert.commandWorked(sessionDB[collName].insert({a: [1, 2]}));

    assert.commandWorked(session.commitTransaction_forTesting());
    session.endSession();

    walkAndAssertReplicaMultikeyConsistency({
        indexType,
        collName,
        indexId: 1, // index 0 is _id, index 1 is the user index.
        multiPathsKey: "a",
        wildcardHint: {"$**": 1},
        expectedMinTimestamps: 1,
        context: indexType + "/same-txn-create/" + collName,
    });
}

verifyIndexCreatedInSameTransaction(IndexType.REGULAR, "coll_regular_same_txn");
verifyIndexCreatedInSameTransaction(IndexType.WILDCARD, "coll_wildcard_same_txn");

rst.stopSet();
