/**
 * Tests that initial sync successfully applies a prepare oplog entry during oplog application phase
 * of initial sync.  Additionally, we will test that a background index build blocks this particular
 * situation until the index build is finished.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */

(function() {
"use strict";

load("jstests/libs/check_log.js");
load("jstests/core/txns/libs/prepare_helpers.js");

const replTest = new ReplSetTest({nodes: 2});
replTest.startSet();

const config = replTest.getReplSetConfig();
// Increase the election timeout so that we do not accidentally trigger an election while the
// secondary is restarting.
config.settings = {
    "electionTimeoutMillis": 12 * 60 * 60 * 1000
};
replTest.initiate(config);

const primary = replTest.getPrimary();
let secondary = replTest.getSecondary();

const dbName = "test";
const collName = "reconstruct_prepared_transactions_initial_sync_index_build";
let testDB = primary.getDB(dbName);
let testColl = testDB.getCollection(collName);

assert.commandWorked(testColl.insert({_id: 0}));

jsTestLog("Restarting the secondary");

// Restart the secondary with startClean set to true so that it goes through initial sync. Also
// restart the node with a failpoint turned on that will pause initial sync. This way we can do
// some writes on the sync source while initial sync is paused and know that its operations
// won't be copied during collection cloning. Instead, the writes must be applied during oplog
// application.
replTest.stop(secondary, undefined /* signal */, {skipValidation: true});
secondary = replTest.start(
    secondary,
    {
        startClean: true,
        setParameter: {
            'failpoint.initialSyncHangDuringCollectionClone': tojson(
                {mode: 'alwaysOn', data: {namespace: testColl.getFullName(), numDocsToClone: 1}}),
            'numInitialSyncAttempts': 1
        }
    },
    true /* wait */);

// Wait for failpoint to be reached so we know that collection cloning is paused.
checkLog.contains(secondary, "initialSyncHangDuringCollectionClone fail point enabled");

jsTestLog("Running operations while collection cloning is paused");

// Perform writes while collection cloning is paused so that we know they must be applied during
// the oplog application stage of initial sync.
assert.commandWorked(testColl.insert({_id: 1, a: 1}));
assert.commandWorked(testColl.createIndex({a: 1}));
// Make the index build hang on the secondary so that initial sync gets to the prepared-txn
// reconstruct stage with the index build still running.
assert.commandWorked(
    secondary.adminCommand({configureFailPoint: 'hangAfterStartingIndexBuild', mode: "alwaysOn"}));

let session = primary.startSession();
let sessionDB = session.getDatabase(dbName);
const sessionColl = sessionDB.getCollection(collName);

jsTestLog("Preparing the transaction");

// Prepare a transaction while collection cloning is paused so that its oplog entry must be
// applied during the oplog application phase of initial sync.
session.startTransaction();
assert.commandWorked(sessionColl.update({_id: 1, a: 1}, {_id: 1, a: 2}));
const prepareTimestamp = PrepareHelpers.prepareTransaction(session, {w: 1});

clearRawMongoProgramOutput();
jsTestLog("Resuming initial sync");

// Resume initial sync.
assert.commandWorked(secondary.adminCommand(
    {configureFailPoint: "initialSyncHangDuringCollectionClone", mode: "off"}));

// Wait for log message.
assert.soon(
    () =>
        rawMongoProgramOutput().indexOf(
            "blocking replication until index builds are finished on test.reconstruct_prepared_transactions_initial_sync_index_build, due to prepared transaction") >=
        0,
    "replication not hanging");

// Unblock index build.
assert.commandWorked(
    secondary.adminCommand({configureFailPoint: 'hangAfterStartingIndexBuild', mode: "off"}));

// Wait for the secondary to complete initial sync.
replTest.awaitSecondaryNodes();

jsTestLog("Initial sync completed");

secondary.setSlaveOk();
const secondaryColl = secondary.getDB(dbName).getCollection(collName);

// Make sure that while reading from the node that went through initial sync, we can't read
// changes to the documents from the prepared transaction after initial sync. Also, make
// sure that the writes that happened when collection cloning was paused happened.
const res = secondaryColl.find().sort({_id: 1}).toArray();
assert.eq(res, [{_id: 0}, {_id: 1, a: 1}], res);

// Wait for the prepared transaction oplog entry to be majority committed before committing the
// transaction.
PrepareHelpers.awaitMajorityCommitted(replTest, prepareTimestamp);

jsTestLog("Committing the transaction");

assert.commandWorked(PrepareHelpers.commitTransaction(session, prepareTimestamp));
replTest.awaitReplication();

// Make sure that we can see the data from the committed transaction on the secondary.
assert.docEq(secondaryColl.findOne({_id: 1}), {_id: 1, a: 2});

replTest.stopSet();
})();
