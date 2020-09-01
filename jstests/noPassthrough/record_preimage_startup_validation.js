/**
 * Validate that we accept/reject the 'recordPreImages' flag via collMod and create commands only if
 * the node is a member of a replica set, and that the flag is only accepted in FCV 4.4.
 *
 * @tags: [requires_replication, requires_persistence]
 */
(function() {
'use strict';

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
 * Start a 2-node repl set so that replication of the 'recordPreImages' field is tested.
 */

let replTest = new ReplSetTest({nodes: 2});
replTest.startSet();
replTest.initiate();

let adminDB = replTest.getPrimary().getDB("admin");
let localDB = replTest.getPrimary().getDB("local");
let testDB = replTest.getPrimary().getDB("test");
const collName1 = "testColl1";
const collName2 = "testColl2";

// Check that we cannot set recordPreImages on the local or admin databases.
assert.commandFailedWithCode(adminDB.runCommand({create: "preimagecoll", recordPreImages: true}),
                             ErrorCodes.InvalidOptions);
assert.commandFailedWithCode(localDB.runCommand({create: "preimagecoll", recordPreImages: true}),
                             ErrorCodes.InvalidOptions);

// Check the create collection command in FCV 4.4 succeeds.
assert.commandWorked(
    testDB.runCommand({create: collName1, recordPreImages: true, writeConcern: {w: 'majority'}}));
assert.eq(findCollectionInfo(testDB, collName1).options.recordPreImages, true);

// Check the collMod collection command in FCV 4.4 succeeds.
assert.commandWorked(testDB.runCommand({create: collName2, writeConcern: {w: 'majority'}}));
assert.commandWorked(
    testDB.runCommand({collMod: collName2, recordPreImages: true, writeConcern: {w: 'majority'}}));
assert.eq(findCollectionInfo(testDB, collName2).options.recordPreImages, true);
assert.commandWorked(
    testDB.runCommand({collMod: collName2, recordPreImages: false, writeConcern: {w: 'majority'}}));
assert.eq(findCollectionInfo(testDB, collName2).options, {});

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
