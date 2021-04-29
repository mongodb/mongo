// Tests timeout behavior of waiting for write concern as well as its interaction with maxTimeMs

(function() {
"use strict";

load("jstests/libs/write_concern_util.js");

var replTest = new ReplSetTest({nodes: 3});
replTest.startSet();
replTest.initiate();
var primary = replTest.getPrimary();
var testDB = primary.getDB('test');
const collName = 'foo';
var testColl = testDB.getCollection(collName);

// The default WC is majority and stopServerReplication will prevent satisfying any majority writes.
assert.commandWorked(primary.adminCommand(
    {setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}));
replTest.awaitReplication();

// Insert a document and implicitly create the collection.
let resetCollection = function(w) {
    assert.commandWorked(
        testColl.insert({_id: 0}, {writeConcern: {w: w, wtimeout: replTest.kDefaultTimeoutMS}}));
    assert.eq(1, testColl.find().itcount());
};

resetCollection(3);

// Make sure that there are only 2 nodes up so w:3 writes will always time out
replTest.stop(2);

// Test wtimeout
var res = testDB.runCommand(
    {insert: collName, documents: [{a: 1}], writeConcern: {w: 3, wtimeout: 1000}});
assert.commandFailedWithCode(res, ErrorCodes.WriteConcernFailed);
assert.eq(ErrorCodes.WriteConcernFailed, res.writeConcernError.code);

// Test maxTimeMS timeout
res = testDB.runCommand(
    {insert: collName, documents: [{a: 1}], writeConcern: {w: 3}, maxTimeMS: 1000});
assert.commandFailedWithCode(res, ErrorCodes.MaxTimeMSExpired);

// Test with wtimeout < maxTimeMS
res = testDB.runCommand({
    insert: collName,
    documents: [{a: 1}],
    writeConcern: {w: 3, wtimeout: 1000},
    maxTimeMS: 10 * 1000
});
assert.commandFailedWithCode(res, ErrorCodes.WriteConcernFailed);
assert.eq(ErrorCodes.WriteConcernFailed, res.writeConcernError.code);

// Test with wtimeout > maxTimeMS
res = testDB.runCommand({
    insert: collName,
    documents: [{a: 1}],
    writeConcern: {w: 3, wtimeout: 10 * 1000},
    maxTimeMS: 1000
});
assert.commandFailedWithCode(res, ErrorCodes.MaxTimeMSExpired);

// dropDatabase respects the 'w' field when it is stronger than the default of majority.
res = testDB.runCommand({dropDatabase: 1, writeConcern: {w: 3, wtimeout: 1000}});
assert.commandFailedWithCode(res, ErrorCodes.WriteConcernFailed);
assert.eq(ErrorCodes.WriteConcernFailed, res.writeConcernError.code);

resetCollection(2);

// Pause the oplog fetcher on secondary so that commit point doesn't advance, meaning that a dropped
// database on the primary will remain in 'drop-pending' state. As there isn't anything in the oplog
// buffer at this time, it is safe to pause the oplog fetcher.
var secondary = replTest.getSecondary();
jsTestLog("Pausing the oplog fetcher on the secondary node.");
stopServerReplication(secondary);

// dropDatabase defaults to 'majority' when a weaker 'w' field is provided, but respects
// 'wtimeout'.
res = testDB.runCommand({dropDatabase: 1, writeConcern: {w: 1, wtimeout: 1000}});
assert.commandFailedWithCode(res, ErrorCodes.WriteConcernFailed);

restartServerReplication(secondary);
replTest.stopSet();
})();
