/**
 * Tests that the replicated fast count for a collection with a prepared (but not yet committed)
 * transaction behaves the same before and after a restart.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_replication,
 *   uses_transactions,
 *   uses_prepare_transaction,
 * ]
 */
import {PrepareHelpers} from "jstests/core/txns/libs/prepare_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {PersistenceProviderUtil} from "jstests/libs/server-rss/persistence_provider_util.js";

const dbName = "test";
const collName = "prepared_txn_count_across_restart";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();
let testDB = primary.getDB(dbName);
let testColl = testDB.getCollection(collName);

if (
    PersistenceProviderUtil.allNodesHavePropertyWithValue(
        testDB,
        "shouldUseReplicatedFastCount",
        false,
    ) &&
    !FeatureFlagUtil.isEnabled(testDB, "ReplicatedFastCount")
) {
    rst.stopSet();
    quit();
}

// Pre-populate the collection with a document so it is non-empty.
assert.commandWorked(testColl.insert({_id: 0, x: "initial"}));
assert.eq(testColl.count(), 1, "expected 1 document after initial insert");

// Start a session and a prepared transaction that inserts two documents.
let session = primary.startSession();
const sessionID = session.getSessionId();
let sessionDB = session.getDatabase(dbName);
let sessionColl = sessionDB.getCollection(collName);

session.startTransaction();
assert.commandWorked(sessionColl.insert({_id: 1, x: "prepared_a"}));
assert.commandWorked(sessionColl.insert({_id: 2, x: "prepared_b"}));
const prepareTimestamp = PrepareHelpers.prepareTransaction(session);

jsTest.log.info("Transaction prepared. Checking count from an external observer before restart.");

// Before restart: with replicated fast count, the external observer does NOT see the prepared
// transaction's inserts. _replicatedNumRecords has not been adjusted (only happens on commit),
// and the external observer's UncommittedFastCountChange is empty.
const countBeforeRestart = testColl.count();
jsTest.log.info("Count before restart (external observer): " + countBeforeRestart);
assert.eq(
    countBeforeRestart,
    1,
    "Before restart, external observer should NOT see prepared transaction inserts in the replicated fast count",
);

// Advance the stable timestamp past the prepare timestamp so that the prepare oplog entry is in
// the stable checkpoint. This ensures the transaction will be reconstructed on restart.
jsTest.log.info("Advancing stable timestamp with a majority write");
assert.commandWorked(
    testColl.runCommand("insert", {documents: [{_id: 100}], writeConcern: {w: "majority"}}),
);

// Verify the majority write is visible but the prepared transaction's inserts still are not
// (to an external observer via replicated fast count).
assert.eq(
    testColl.count(),
    2,
    "After majority write, external observer should see 2 docs (initial + majority), not the prepared inserts",
);

jsTest.log.info("Restarting the node");
rst.stop(primary, undefined, {skipValidation: true});
rst.start(primary, {}, true);

primary = rst.getPrimary();
testDB = primary.getDB(dbName);
testColl = testDB.getCollection(collName);

jsTest.log.info("Node restarted. Checking count from an external observer after restart");

assert.eq(
    testColl.count(),
    2,
    "After restart, external observer should see only 2 docs because the prepared txn is not yet committed",
);

// Verify the prepared documents are NOT yet readable.
const visibleDocs = testColl.find().toArray();
assert.sameMembers(
    visibleDocs.map((d) => d._id),
    [0, 100],
    "Only non-prepared documents should be readable, but found: " + tojson(visibleDocs),
);

jsTest.log.info("Now committing the recovered prepared transaction");

// Wait for the prepare oplog entry to be majority committed. commitTransaction for a prepared
// transaction requires this; otherwise the server rejects the command.
PrepareHelpers.awaitMajorityCommitted(rst, prepareTimestamp);

// Reconnect to the session and commit the prepared transaction.
session = PrepareHelpers.createSessionWithGivenId(primary, sessionID);
sessionDB = session.getDatabase(dbName);
session.setTxnNumber_forTesting(0);
const txnNumber = session.getTxnNumber_forTesting();

assert.commandWorked(
    sessionDB.adminCommand({
        commitTransaction: 1,
        commitTimestamp: prepareTimestamp,
        txnNumber: NumberLong(txnNumber),
        autocommit: false,
    }),
);

const countAfterCommit = testColl.count();
jsTest.log.info("Count after commit: " + countAfterCommit);
assert.eq(
    countAfterCommit,
    4,
    "After commit, count should be 4: 1 (initial) + 2 (committed) + 1 (majority)",
);

// All documents are now readable. There are 4 documents.
const allDocs = testColl.find().sort({_id: 1}).toArray();
assert.eq(allDocs.length, 4, "Expected 4 actual documents after commit, got " + allDocs.length);
assert.sameMembers(
    allDocs.map((d) => d._id),
    [0, 1, 2, 100],
    "Unexpected document _ids: " + tojson(allDocs),
);

rst.stopSet();
