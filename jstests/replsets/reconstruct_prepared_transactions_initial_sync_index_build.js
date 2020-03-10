/**
 * Tests that initial sync successfully applies a prepare oplog entry during oplog application phase
 * of initial sync.  Additionally, we will test that a background index build interleaves without
 * hanging.
 *
 * @tags: [
 *     uses_transactions,
 *     uses_prepare_transaction,
 *     requires_fcv_44,
 * ]
 */

(function() {
"use strict";

load("jstests/core/txns/libs/prepare_helpers.js");
load("jstests/libs/fail_point_util.js");
load('jstests/noPassthrough/libs/index_build.js');

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
assert.commandWorked(secondary.adminCommand({
    waitForFailPoint: "initialSyncHangDuringCollectionClone",
    timesEntered: 1,
    maxTimeMS: kDefaultWaitForFailPointTimeout
}));

jsTestLog("Running operations while collection cloning is paused");

// Perform writes while collection cloning is paused so that we know they must be applied during
// the oplog application stage of initial sync.
assert.commandWorked(testColl.insert({_id: 1, a: 1}));

if (!IndexBuildTest.supportsTwoPhaseIndexBuild(primary)) {
    // Make the index build hang on the secondary so that initial sync gets to the prepared-txn
    // reconstruct stage with the index build still running.
    jsTest.log("Hanging index build on the secondary node");
    IndexBuildTest.pauseIndexBuilds(secondary);

    jsTest.log("Beginning index build");
    assert.commandWorked(testColl.createIndex({a: 1}));
} else {
    // Make the index build hang on the primary so that only a startIndexBuild oplog entry is
    // replicated and initial sync gets to the prepared-txn reconstruct stage with the index build
    // still running.
    jsTest.log("Hanging index build on the primary node");
    IndexBuildTest.pauseIndexBuilds(primary);

    jsTest.log("Beginning index build");
    IndexBuildTest.startIndexBuild(primary, testColl.getFullName(), {a: 1});
}

let session = primary.startSession();
let sessionDB = session.getDatabase(dbName);
const sessionColl = sessionDB.getCollection(collName);

jsTestLog("Preparing the transaction");

// Prepare a transaction while collection cloning is paused so that its oplog entry must be
// applied during the oplog application phase of initial sync.
session.startTransaction();
assert.commandWorked(sessionColl.update({_id: 1, a: 1}, {_id: 1, a: 2}));
const prepareTimestamp = PrepareHelpers.prepareTransaction(session, {w: 1});

jsTestLog("Resuming initial sync");

// Resume initial sync.
assert.commandWorked(secondary.adminCommand(
    {configureFailPoint: "initialSyncHangDuringCollectionClone", mode: "off"}));

// Unblock index build.
if (!IndexBuildTest.supportsTwoPhaseIndexBuild(primary)) {
    // Wait for log message.
    checkLog.containsJson(secondary, 21849, {ns: testColl.getFullName()});

    // Let the secondary finish its index build.
    IndexBuildTest.resumeIndexBuilds(secondary);
} else {
    // Let the primary finish its index build and replicate a commit to the secondary.
    IndexBuildTest.resumeIndexBuilds(primary);
}

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
