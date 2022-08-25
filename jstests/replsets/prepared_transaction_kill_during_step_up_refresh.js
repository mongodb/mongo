/**
 * Tests that the work to restore locks for prepared transactions on step up is not killable via
 * killSessions commands.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */
(function() {
"use strict";
load("jstests/core/txns/libs/prepare_helpers.js");
load("jstests/libs/fail_point_util.js");
load("jstests/replsets/rslib.js");  // For reconnect()

const rst = new ReplSetTest({nodes: 2, name: jsTestName()});
rst.startSet();
rst.initiateWithHighElectionTimeout();

const dbName = "primaryDB";
const collName = "testcoll";

const primary = rst.getPrimary();
const newPrimary = rst.getSecondary();

const primaryDB = primary.getDB(dbName);
const primaryColl = primaryDB.getCollection(collName);
assert.commandWorked(primaryDB.runCommand({create: collName, writeConcern: {w: "majority"}}));

jsTestName("Starting a transaction");
const session = primary.startSession({causalConsistency: false});
session.startTransaction({writeConcern: {w: "majority"}});
const lsid = session.getSessionId().id;

jsTestLog("LSID for our session is " + tojson(lsid));

jsTestLog("Inserting a doc in a transaction.");
const doc = {
    _id: "txnDoc"
};
assert.commandWorked(session.getDatabase(dbName).getCollection(collName).insert(doc));

jsTestLog("Putting transaction into prepare");
const prepareTimestamp = PrepareHelpers.prepareTransaction(session);

jsTestLog("Setting failpoint on new primary");
const stepUpFP = configureFailPoint(newPrimary, "hangDuringStepUpPrepareRestoreLocks");

jsTestLog("Stepping up new primary");
rst.stepUp(newPrimary, {awaitWritablePrimary: false});
reconnect(primary);

jsTestLog("Waiting on new primary to hit step up failpoint");
stepUpFP.wait();

jsTestLog("Killing the session");
const newPrimaryDB = newPrimary.getDB(dbName);
assert.commandWorked(newPrimaryDB.runCommand({killSessions: [{id: lsid}]}));

jsTestLog("Allowing step up to continue");
stepUpFP.off();
assert(newPrimary, rst.getPrimary());

jsTestLog("Committing transaction on the new primary");
// Create a proxy session to reuse the session state of the old primary.
const newSession = new _DelegatingDriverSession(newPrimary, session);

assert.commandWorked(PrepareHelpers.commitTransaction(newSession, prepareTimestamp));

assert.eq(doc, primaryColl.findOne({}), primaryColl.find({}).toArray());

rst.stopSet();
})();
