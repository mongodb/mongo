/*
 * This test validates that we accept the 'recordPreImage' flag via the collMode and create commands
 * only if the node is a member of a replica set that is not a shard/config server. Also tests that
 * this flag cannot be set on collections in the 'local' or 'admin' databases.
 *
 * @tags: [requires_replication, requires_persistence]
 */
(function() {
'use strict';

// Start a mongod that's not in a replica set
let conn = MongoRunner.runMongod({});

let testDB = conn.getDB("test");

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

// Check that we cannot record pre-images on a standalone.
assert.commandFailedWithCode(testDB.runCommand({create: "test", recordPreImages: true}),
                             ErrorCodes.InvalidOptions);
// Check that failing to set the option doesn't accidentally set it anyways.
assert.eq(findCollectionInfo("test").options, undefined);

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

testDB = rsTest.getPrimary().getDB("test");

// Check the positive test cases. We should be able to set this flag via create or collMod.
assert.commandWorked(testDB.runCommand({create: "test", recordPreImages: true}));
assert.eq(findCollectionInfo("test").options.recordPreImages, true);

assert.commandWorked(testDB.runCommand({create: "test2"}));
assert.commandWorked(testDB.runCommand({collMod: "test2", recordPreImages: true}));
assert.eq(findCollectionInfo("test2").options.recordPreImages, true);

// Test that the flag can be unset successfully using the 'collMod' command.
assert.commandWorked(testDB.runCommand({collMod: "test", recordPreImages: false}));
assert.eq(findCollectionInfo("test").options, {});

// Re-enable the flag and test that the replica set node can restart successfully.
assert.commandWorked(testDB.runCommand({collMod: "test", recordPreImages: true}));
rsTest.restart(rsTest.getPrimary());

rsTest.stopSet();
}());
