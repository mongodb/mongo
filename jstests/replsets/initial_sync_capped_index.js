/**
 * Test to ensure that initial sync builds indexes correctly when syncing a capped collection that
 * is receiving concurrent inserts.
 *
 * The main goal of this test is to have the SECONDARY clone more documents than would actually fit
 * in a specific capped collection, leading to the deletion of documents (i.e. 'capping') on the
 * SECONDARY *during* the collection cloning process. This scenario is encountered when a SECONDARY
 * opens a cursor on a capped collection, begins iterating on that cursor, and, before the cursor is
 * exhausted, new documents get appended to the capped collection that it is cloning.
 *
 * Test Setup:
 * 1-node replica set that is reconfigured to a 2-node replica set.
 *
 * 1. Initiate replica set.
 * 2. Create a capped collection on the PRIMARY and overflow it.
 * 4. Add a SECONDARY node to the replica set.
 * 5. Set fail point on SECONDARY that hangs capped collection clone after first 'find' response.
 * 6. Let SECONDARY start initial sync.
 * 7. Wait for initial 'find' response during the cloning of the capped collection.
 * 8. Insert documents to the capped collection on the PRIMARY.
 * 9, Disable fail point on SECONDARY so the rest of the capped collection documents are cloned.
 * 8. Once initial sync completes, ensure that capped collection indexes on the SECONDARY are valid.
 *
 * This is a regression test for SERVER-29197.
 *
 * @tags: [
 *   uses_full_validation,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {waitForState} from "jstests/replsets/rslib.js";

// Set up replica set.
var testName = "initial_sync_capped_index";
var dbName = testName;
var replTest = new ReplSetTest({name: testName, nodes: 1});
replTest.startSet();
replTest.initiate();

var primary = replTest.getPrimary();
var primaryDB = primary.getDB(dbName);
var cappedCollName = "capped_coll";
var primaryCappedColl = primaryDB[cappedCollName];

const cappedMaxCount = 5;
const additionalDocumentCount = 2;

jsTestLog(`Creating capped collection with max ${cappedMaxCount} documents`);
assert.commandWorked(
    primaryDB.createCollection(cappedCollName, {capped: true, size: 4096, max: cappedMaxCount}));
assert.commandWorked(primaryCappedColl.createIndex({a: 1}));

jsTestLog(
    `Inserting ${cappedMaxCount} documents so that the next insertion will delete a document`);
for (let i = 0; i < cappedMaxCount; ++i) {
    assert.commandWorked(primaryCappedColl.insert({_id: i, a: i}));
}

// Add a SECONDARY node. It should use batchSize=3 for its initial sync queries. The batch size
// needs to be greater than `additionalDocumentCount` or the initial sync will fail and restart
// rather than inserting too many documents.
jsTestLog("Adding secondary node.");
replTest.add({rsConfig: {votes: 0, priority: 0}, setParameter: "collectionClonerBatchSize=3"});

var secondary = replTest.getSecondary();
var collectionClonerFailPoint = "initialSyncHangCollectionClonerAfterHandlingBatchResponse";

// Make the collection cloner pause after its initial 'find' response on the capped collection.
var nss = dbName + "." + cappedCollName;
jsTestLog("Enabling collection cloner fail point for " + nss);
let failPoint = configureFailPoint(secondary, collectionClonerFailPoint, {nss: nss});

// Let the SECONDARY begin initial sync.
jsTestLog("Re-initiating replica set with new secondary.");
replTest.reInitiate();

jsTestLog("Waiting for the initial 'find' response of capped collection cloner to complete.");
failPoint.wait();

// Append documents to the capped collection so that the SECONDARY will clone these
// additional documents.
for (let i = cappedMaxCount; i < cappedMaxCount + additionalDocumentCount; ++i) {
    assert.commandWorked(primaryCappedColl.insert({_id: i, a: i}));
}

// Let the 'getMore' requests for the capped collection clone continue.
jsTestLog("Disabling collection cloner fail point for " + nss);
failPoint.off();

// Wait until initial sync completes.
replTest.awaitReplication();

// Before validating the secondary, confirm that it is in the SECONDARY state. Otherwise, the
// validate command will fail.
waitForState(secondary, ReplSetTest.State.SECONDARY);

// Make sure the indexes created during initial sync are valid.
var secondaryCappedColl = secondary.getDB(dbName)[cappedCollName];
var validate_result = secondaryCappedColl.validate({full: true});
var failMsg =
    "Index validation of '" + secondaryCappedColl.name + "' failed: " + tojson(validate_result);
assert(validate_result.valid, failMsg);

// Verify that the replicated collection has the expected documents and querying on the indexes
// works
for (let i = 0; i < additionalDocumentCount; ++i) {
    assert(!secondaryCappedColl.findOne({_id: i}));
    assert(!secondaryCappedColl.findOne({a: i}));
}
for (let i = additionalDocumentCount; i < cappedMaxCount + additionalDocumentCount; ++i) {
    assert(secondaryCappedColl.findOne({_id: i}));
    assert(secondaryCappedColl.findOne({a: i}));
}

replTest.stopSet();
