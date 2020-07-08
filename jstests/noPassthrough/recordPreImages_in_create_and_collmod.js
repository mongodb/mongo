/*
 * Tests the 'recordPreImage' flag is settable via the collMode and create commands. Also tests that
 * this flag cannot be set on collections in the 'local' or 'admin' databases.
 *
 * @tags: [requires_replication]
 */
(function() {
'use strict';

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

let rsTest = new ReplSetTest({nodes: 1});
rsTest.startSet();
rsTest.initiate();

const dbName = 'testDB';
const collName = 'recordPreImageColl';
const collName2 = 'recordPreImageColl2';

let primary = rsTest.getPrimary();
let adminDB = primary.getDB("admin");
let localDB = primary.getDB("local");
let testDB = primary.getDB(dbName);

// Check that we cannot set recordPreImages on the local or admin databases.
assert.commandFailedWithCode(adminDB.runCommand({create: collName, recordPreImages: true}),
                             ErrorCodes.InvalidOptions);
assert.commandFailedWithCode(localDB.runCommand({create: collName, recordPreImages: true}),
                             ErrorCodes.InvalidOptions);

// We should be able to set the recordPreImages flag via create or collMod.
assert.commandWorked(testDB.runCommand({create: collName, recordPreImages: true}));
assert.eq(findCollectionInfo(collName).options.recordPreImages, true);

assert.commandWorked(testDB.runCommand({create: collName2}));
assert.commandWorked(testDB.runCommand({collMod: collName2, recordPreImages: true}));
assert.eq(findCollectionInfo(collName2).options.recordPreImages, true);

// Test that the recordPreImages flag can be unset successfully using the 'collMod' command.
assert.commandWorked(testDB.runCommand({collMod: collName, recordPreImages: false}));
assert.eq(findCollectionInfo(collName).options, {});

rsTest.stopSet();
}());
