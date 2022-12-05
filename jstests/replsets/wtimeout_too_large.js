/**
 * Test that the server rejects extremely large values for write concern wtimeout.
 */

(function() {
"use strict";

const rst = new ReplSetTest({name: jsTestName(), nodes: 2});
rst.startSet();
rst.initiateWithHighElectionTimeout();

const dbName = "testdb";
const collName = "testcoll";

const primary = rst.getPrimary();
const primaryDB = primary.getDB(dbName);
const primaryColl = primaryDB.getCollection(collName);

jsTestLog("Issuing a write within accepted wtimeout bounds");
assert.commandWorked(
    primaryColl.insert({a: 1}, {writeConcern: {w: 2, wtimeout: ReplSetTest.kDefaultTimeoutMS}}));

jsTestLog("Issuing a high wtimeout write and confirming that it gets rejected");

// Outside int32 bounds.
const oneTrillionMS = 1000 * 1000 * 1000 * 1000;
assert.commandFailedWithCode(
    primaryColl.insert({b: 2}, {writeConcern: {w: 2, wtimeout: oneTrillionMS}}),
    ErrorCodes.FailedToParse);

rst.stopSet();
})();
