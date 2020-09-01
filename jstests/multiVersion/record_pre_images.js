/**
 * Test 'recordPreImages' behavior in FCV 4.2 with a v4.4 binary primary and v4.2 binary secondary.
 * Check that nothing the v4.4 binary primary does in FCV 4.4 can crash the v4.2 secondary.
 */

(function() {
"use strict";

jsTestLog("Starting 2 node replica set.");

let rst = new ReplSetTest({
    name: "record_pre_image",
    nodes: [{binVersion: "latest"}, {binVersion: "last-stable", rsConfig: {priority: 0}}]
});
rst.startSet();
rst.initiate();

let primaryDB = rst.getPrimary().getDB('test');

const collName1 = "testColl1";

jsTestLog("Starting 'recordPreImages' testing.");

// Check the create collection command in FCV 4.2 fails.
// Note: create cmd with {recordPreImages: false} can succeed, but has no effect because it sets
// nothing.
assert.commandFailedWithCode(primaryDB.runCommand({create: collName1, recordPreImages: true}),
                             ErrorCodes.InvalidOptions);
assert.commandWorked(primaryDB.runCommand(
    {create: collName1, recordPreImages: false, writeConcern: {w: 'majority'}}));

// Check the collMod command in FCV 4.2 fails.
assert.writeOK(primaryDB.getCollection(collName1).insert({_id: 1}));  // create the collection.
assert.commandFailedWithCode(primaryDB.runCommand({collMod: collName1, recordPreImages: false}),
                             ErrorCodes.InvalidOptions);
assert.commandFailedWithCode(primaryDB.runCommand({collMod: collName1, recordPreImages: true}),
                             ErrorCodes.InvalidOptions);

rst.stopSet();
})();
