/**
 * Validate that we accept/reject the 'recordPreImages' flag via collMod and create commands only if
 * the node is a member of a replica set, and that the flag is only accepted in FCV 4.4.
 *
 * @tags: [requires_replication, requires_persistence]
 */
(function() {
'use strict';

/**
 * Test that a standalone cannot record pre-images.
 */

let conn = MongoRunner.runMongod({});
assert.commandFailedWithCode(conn.getDB("test").runCommand({create: "test", recordPreImages: true}),
                             ErrorCodes.InvalidOptions);
MongoRunner.stopMongod(conn);

/**
 * Find the collection information on database 'nodeDB' for collection 'collName'.
 */
const findCollectionInfo = function(nodeDB, collName) {
    let collInfos = nodeDB.getCollectionInfos();
    if (collInfos.length == 0) {
        return {};
    }
    let collInfo = collInfos.filter(function(z) {
        return z.name == collName;
    });
    assert.eq(collInfo.length, 1);
    return collInfo[0];
};

/**
 * Start a 1-node repl set to be used for the rest of the test.
 */

let replTest = new ReplSetTest({nodes: 1});
replTest.startSet();
replTest.initiate();

let adminDB = replTest.getPrimary().getDB("admin");
let localDB = replTest.getPrimary().getDB("local");
let testDB = replTest.getPrimary().getDB("test");
const collName1 = "testColl1";
const collName2 = "testColl2";
const collName3 = "testColl3";

// Check that we cannot set recordPreImages on the local or admin databases.
assert.commandFailedWithCode(adminDB.runCommand({create: "preimagecoll", recordPreImages: true}),
                             ErrorCodes.InvalidOptions);
assert.commandFailedWithCode(localDB.runCommand({create: "preimagecoll", recordPreImages: true}),
                             ErrorCodes.InvalidOptions);

// Should not be able to set or unset recordPreImages in lastStableFCV 4.2. Creating oplog entries
// with the new recordPreImages field would crash v4.2 secondaries to which the field is unknown.
assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: lastStableFCV}));

// Check the create collection command in FCV 4.2 fails.
// Note: recordPreImages: false can succeed, but has no effect because it sets nothing.
assert.commandFailedWithCode(testDB.runCommand({create: collName1, recordPreImages: true}),
                             ErrorCodes.InvalidOptions);

// Check the collMod command in FCV 4.2 fails.
assert.writeOK(testDB.getCollection(collName1).insert({_id: 1}));  // create the collection.
assert.commandFailedWithCode(testDB.runCommand({collMod: collName1, recordPreImages: true}),
                             ErrorCodes.InvalidOptions);
// Check that failing to set the option doesn't accidentally set it anyways.
assert.eq(findCollectionInfo(testDB, collName1).options, {});

// Check the positive test cases. We should be able to set the recordPreImages field via create or
// collMod in FCV 4.4.
assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: latestFCV}));

// Check the create collection command in FCV 4.4 succeeds.
assert.commandWorked(testDB.runCommand({create: collName2, recordPreImages: true}));
assert.eq(findCollectionInfo(testDB, collName2).options.recordPreImages, true);

// Check the collMod collection command in FCV 4.4 succeeds.
assert.commandWorked(testDB.runCommand({create: collName3}));
assert.commandWorked(testDB.runCommand({collMod: collName3, recordPreImages: true}));
assert.eq(findCollectionInfo(testDB, collName3).options.recordPreImages, true);
assert.commandWorked(testDB.runCommand({collMod: collName3, recordPreImages: false}));
assert.eq(findCollectionInfo(testDB, collName3).options, {});

// Restarting the server while in FCV 4.4 when recordPreImages flags are set should be successful.
assert.doesNotThrow(() => {
    replTest.restart(replTest.getPrimary());
});

// The server should fail to restart when the recordPreImages field is found on startup in FCV 4.2.
assert.commandWorked(replTest.getPrimary().getDB("admin").runCommand(
    {setFeatureCompatibilityVersion: lastStableFCV}));
assert.throws(() => {
    replTest.restart(replTest.getPrimary());
});

replTest.stopSet();
}());
