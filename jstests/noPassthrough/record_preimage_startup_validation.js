/*
 * This test validates that we accept/reject the recordPreImage flag via collMode and create
 * commands only if the node is a member of a replica set that is not a shard/config server,
 * and that we require the FCV to be 4.4.
 *
 * @tags: [requires_replication, requires_persistence]
 */
(function() {
'use strict';

// Start a mongod that's not in a replica set
let conn = MongoRunner.runMongod({});

// Check that we cannot record pre-images on a standalone
assert.commandFailedWithCode(conn.getDB("test").runCommand({create: "test", recordPreImages: true}),
                             ErrorCodes.InvalidOptions);

MongoRunner.stopMongod(conn);

// Start a 1-node repl set to be used for the rest of the test
let rsTest = new ReplSetTest({nodes: 1});
rsTest.startSet();
rsTest.initiate();

// Check that we cannot set recordPreImages on the local or admin databases.
let adminDB = rsTest.getPrimary().getDB("admin");
assert.commandFailedWithCode(adminDB.runCommand({create: "preimagecoll", recordPreImages: true}),
                             ErrorCodes.InvalidOptions);
let localDB = rsTest.getPrimary().getDB("local");
assert.commandFailedWithCode(localDB.runCommand({create: "preimagecoll", recordPreImages: true}),
                             ErrorCodes.InvalidOptions);

let testDB = rsTest.getPrimary().getDB("test");
const findCollectionInfo = function(collName) {
    var all = testDB.getCollectionInfos();
    if (all.length == 0) {
        return {};
    }
    all = all.filter(function(z) {
        return z.name == collName;
    });
    assert.eq(all.length, 1);
    return all[0];
};

assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: lastStableFCV}));

// This should fail because we've set the FCV to lastStableFCV (4.2).
assert.commandFailedWithCode(testDB.runCommand({create: "test", recordPreImages: true}),
                             ErrorCodes.InvalidOptions);
// Check that failing to set the option doesn't accidentally set it anyways.
assert.eq(findCollectionInfo("test").options, undefined);

assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: latestFCV}));

// Check the positive test cases. We should be able to set this flag via create or collMod.
assert.commandWorked(testDB.runCommand({create: "test", recordPreImages: true}));
assert.eq(findCollectionInfo("test").options.recordPreImages, true);

assert.commandWorked(testDB.runCommand({create: "test2"}));
assert.commandWorked(testDB.runCommand({collMod: "test2", recordPreImages: true}));
assert.eq(findCollectionInfo("test2").options.recordPreImages, true);

// Downgrade the FCV
assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: lastStableFCV}));

// We should be able to unset the flag when the FCV is downgraded
assert.commandWorked(testDB.runCommand({collMod: "test", recordPreImages: false}));
// But we shouldn't be able to set it.
assert.commandFailedWithCode(testDB.runCommand({collMod: "test", recordPreImages: true}),
                             ErrorCodes.InvalidOptions);

// The server should fail to start with any collections having the flag set.
assert.throws(() => {
    rsTest.restart(rsTest.getPrimary());
});

rsTest.stopSet();
}());
(function() {
rsTest = new ReplSetTest({nodes: 1});
rsTest.startSet();
rsTest.initiate();
adminDB = rsTest.getPrimary().getDB("admin");
testDB = rsTest.getPrimary().getDB("test");

assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: latestFCV}));
assert.commandWorked(testDB.runCommand({create: "test", recordPreImages: true}));

assert.doesNotThrow(() => {
    rsTest.restart(rsTest.getPrimary());
});

rsTest.stopSet();
}());
