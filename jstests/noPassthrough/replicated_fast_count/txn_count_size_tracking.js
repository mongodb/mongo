/**
 * Tests that collection count and size are tracked correctly during and after non-prepared
 * multi-document transactions. Specifically:
 *   - During a transaction, external observers see only committed state.
 *   - After a commit, count and size reflect the transaction's writes.
 *   - After an abort, count and size remain at the pre-transaction committed state.
 *
 * Each scenario is exercised with inserts, updates, and deletes, and for each with both a regular
 * transaction (single oplog entry) and a large transaction split into chained applyOps entries.
 *
 * @tags: [
 *   featureFlagReplicatedFastCount,
 *   requires_replication,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

const dbName = "test";

// Fix maxNumberOfTransactionOperationsInSingleOplogEntry=6 at startup so that:
//   - Regular single-type transactions (2 ops × 2 collections = 4 ops) fit in a single entry.
//   - Regular mixed transactions (1 op × 3 types × 2 collections = 6 ops) fit in a single entry.
//   - Chained single-type transactions (12 ops × 2 collections = 24 ops / 6 = 4 entries).
//   - Chained mixed transactions (4 ops × 3 types × 2 collections = 24 ops / 6 = 4 entries).
const rst = new ReplSetTest({
    nodes: 1,
    nodeOptions: {setParameter: {maxNumberOfTransactionOperationsInSingleOplogEntry: 6}},
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const testDB = primary.getDB(dbName);

function getStats(coll) {
    return assert.commandWorked(testDB.runCommand({collStats: coll.getName()}));
}

/**
 * Drops both collections and pre-populates them with 12 identical documents (_id 1–12).
 * Returns the initial document count per collection.
 */
function setupCollections(coll1, coll2) {
    coll1.drop();
    coll2.drop();
    const docs = Array.from({length: 12}, (_, i) => ({_id: i + 1, x: "a"}));
    assert.commandWorked(coll1.insertMany(docs));
    assert.commandWorked(coll2.insertMany(docs));
    return docs.length;
}

/**
 * Executes `opCount` write operations of the given type on both session-bound collections within
 * an already-open transaction. Returns {countDelta, sizeDelta} reflecting the net change each
 * collection will have after the transaction commits.
 *
 *  "insert" — inserts opCount new documents (_id 100, 101, …)
 *  "update" — expands the first opCount existing documents with a large payload field
 *  "delete" — removes the first opCount existing documents by _id
 */
function doTxnOps(sessionColl1, sessionColl2, opType, opCount) {
    let countDelta = 0;
    let sizeDelta = 0;
    for (let i = 0; i < opCount; i++) {
        if (opType === "insert") {
            const doc = {_id: 100 + i, val: i};
            assert.commandWorked(sessionColl1.insert(doc));
            assert.commandWorked(sessionColl2.insert(doc));
            countDelta += 1;
            sizeDelta += Object.bsonsize(doc);
        } else if (opType === "update") {
            assert.commandWorked(sessionColl1.update({_id: i + 1}, {$set: {payload: "x".repeat(200)}}));
            assert.commandWorked(sessionColl2.update({_id: i + 1}, {$set: {payload: "x".repeat(200)}}));
            sizeDelta +=
                Object.bsonsize({_id: i + 1, x: "a", payload: "x".repeat(200)}) - Object.bsonsize({_id: i + 1, x: "a"});
        } else {
            // "delete"
            const doc = {_id: i + 1, x: "a"};
            assert.commandWorked(sessionColl1.remove({_id: i + 1}));
            assert.commandWorked(sessionColl2.remove({_id: i + 1}));
            countDelta -= 1;
            sizeDelta -= Object.bsonsize(doc);
        }
    }
    return {countDelta, sizeDelta};
}

/**
 * Executes a mixed transaction with inserts, updates, and deletes on both collections within an
 * already-open transaction. Uses non-overlapping doc IDs to avoid conflicts between op types:
 *   inserts  — new docs _id 100, 101, …
 *   updates  — existing docs _id 1..opCount (add payload field)
 *   deletes  — existing docs _id 12, 11, … (from the end, away from updated docs)
 *
 * Returns {countDelta, sizeDelta} reflecting the net change each collection will have after commit.
 */
function doMixedTxnOps(sessionColl1, sessionColl2, opCount) {
    let countDelta = 0;
    let sizeDelta = 0;

    for (let i = 0; i < opCount; i++) {
        const doc = {_id: 100 + i, val: i};
        assert.commandWorked(sessionColl1.insert(doc));
        assert.commandWorked(sessionColl2.insert(doc));
        countDelta += 1;
        sizeDelta += Object.bsonsize(doc);
    }

    for (let i = 0; i < opCount; i++) {
        assert.commandWorked(sessionColl1.update({_id: i + 1}, {$set: {payload: "x".repeat(200)}}));
        assert.commandWorked(sessionColl2.update({_id: i + 1}, {$set: {payload: "x".repeat(200)}}));
        sizeDelta +=
            Object.bsonsize({_id: i + 1, x: "a", payload: "x".repeat(200)}) - Object.bsonsize({_id: i + 1, x: "a"});
    }

    for (let i = 0; i < opCount; i++) {
        const id = 12 - i;
        assert.commandWorked(sessionColl1.remove({_id: id}));
        assert.commandWorked(sessionColl2.remove({_id: id}));
        countDelta -= 1;
        sizeDelta -= Object.bsonsize({_id: id, x: "a"});
    }

    return {countDelta, sizeDelta};
}

/**
 * Runs one test scenario.
 *
 * @param {string}  testName         - Label printed in the log.
 * @param {boolean} commit           - true to commit the transaction, false to abort it.
 * @param {boolean} chainedApplyOps  - true to produce multiple applyOps oplog entries.
 * @param {string}  opType           - "insert", "update", or "delete".
 */
function runTest(testName, {commit, chainedApplyOps, opType}) {
    jsTestLog(`${testName}: commit=${commit}, chainedApplyOps=${chainedApplyOps}, opType=${opType}`);

    const coll1 = testDB["txn_count_size_1"];
    const coll2 = testDB["txn_count_size_2"];
    const baseCount = setupCollections(coll1, coll2);

    const base1 = getStats(coll1);
    const base2 = getStats(coll2);
    assert.eq(baseCount, base1.count);
    assert.eq(baseCount, base2.count);

    const session = primary.startSession({causalConsistency: false});
    const sessionColl1 = session.getDatabase(dbName)[coll1.getName()];
    const sessionColl2 = session.getDatabase(dbName)[coll2.getName()];

    // opCount controls how many of each operation are performed per collection.
    // For single op types:
    //   chained: 12 ops × 2 collections = 24 ops / 6 per entry = 4 chained applyOps
    //   regular: 2 ops × 2 collections = 4 ops / 6 per entry = 1 applyOps (single entry)
    // For mixed (3 op types):
    //   chained: 4 ops × 3 types × 2 collections = 24 ops / 6 per entry = 4 chained applyOps
    //   regular: 1 op × 3 types × 2 collections = 6 ops / 6 per entry = 1 applyOps (single entry)
    const opCount = opType === "mixed" ? (chainedApplyOps ? 4 : 1) : chainedApplyOps ? 12 : 2;

    session.startTransaction();
    const {countDelta, sizeDelta} =
        opType === "mixed"
            ? doMixedTxnOps(sessionColl1, sessionColl2, opCount)
            : doTxnOps(sessionColl1, sessionColl2, opType, opCount);

    // External observers must see only the committed state.
    const mid1 = getStats(coll1);
    const mid2 = getStats(coll2);
    assert.eq(base1.count, mid1.count, "external coll1 count must be unchanged mid-transaction");
    assert.eq(base2.count, mid2.count, "external coll2 count must be unchanged mid-transaction");
    assert.eq(base1.size, mid1.size, "external coll1 size must be unchanged mid-transaction");
    assert.eq(base2.size, mid2.size, "external coll2 size must be unchanged mid-transaction");

    if (commit) {
        assert.commandWorked(session.commitTransaction_forTesting());

        const post1 = getStats(coll1);
        const post2 = getStats(coll2);

        const expectedCount = baseCount + countDelta;
        assert.eq(
            expectedCount,
            post1.count,
            `incorrect count after committed ${opType}s, expected ${expectedCount} got ${post1.count} for coll1`,
        );
        assert.eq(
            expectedCount,
            post2.count,
            `incorrect count after committed ${opType}s, expected ${expectedCount} got ${post2.count} for coll2`,
        );

        const expectedSize = base1.size + sizeDelta;
        assert.eq(
            expectedSize,
            post1.size,
            `incorrect size after committed ${opType}s, expected ${expectedSize} got ${post1.size} for coll1`,
        );
        assert.eq(
            expectedSize,
            post2.size,
            `incorrect size after committed ${opType}s, expected ${expectedSize} got ${post2.size} for coll2`,
        );

        if (chainedApplyOps) {
            // Confirm the oplog contains exactly 4 chained applyOps entries for this transaction.
            const txnNum = session.getTxnNumber_forTesting();
            const sessionId = session.getSessionId().id;
            const txnOps = primary.getDB("local")["oplog.rs"].find({"lsid.id": sessionId, txnNumber: txnNum}).toArray();
            assert.eq(txnOps.length, 4, "expected 4 chained applyOps entries; got: " + tojson(txnOps));
        }
    } else {
        assert.commandWorked(session.abortTransaction_forTesting());

        const post1 = getStats(coll1);
        const post2 = getStats(coll2);
        assert.eq(base1.count, post1.count, "coll1 count must be unchanged after abort");
        assert.eq(base2.count, post2.count, "coll2 count must be unchanged after abort");
        assert.eq(base1.size, post1.size, "coll1 size must be unchanged after abort");
        assert.eq(base2.size, post2.size, "coll2 size must be unchanged after abort");
    }

    session.endSession();
}

for (const opType of ["insert", "update", "delete", "mixed"]) {
    for (const chainedApplyOps of [false, true]) {
        const mode = chainedApplyOps ? "chained applyOps" : "regular";
        runTest(`${opType} — commit (${mode})`, {commit: true, chainedApplyOps, opType});
        runTest(`${opType} — abort (${mode})`, {commit: false, chainedApplyOps, opType});
    }
}

rst.stopSet();
