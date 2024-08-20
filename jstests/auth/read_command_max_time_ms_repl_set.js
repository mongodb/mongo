/**
 * Tests that 'defaultMaxTimeMS' is applied correctly to the read commands.
 *
 * @tags: [
 *   requires_replication,
 *   requires_auth,
 *   # Transactions aborted upon fcv upgrade or downgrade; cluster parameters use internal txns.
 *   uses_transactions,
 *   requires_fcv_80,
 *   # Uses $where operator
 *   requires_scripting,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({nodes: 3, keyFile: "jstests/libs/key1"});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const dbName = jsTestName();
const adminDB = primary.getDB("admin");

// Create the admin user, which is used to insert.
adminDB.createUser({user: 'admin', pwd: 'admin', roles: ['root']});
assert.eq(1, adminDB.auth("admin", "admin"));

const testDB = adminDB.getSiblingDB(dbName);
const collName = "test";
const coll = testDB.getCollection(collName);

for (let i = 0; i < 10; ++i) {
    // Ensures the documents are visible on all nodes.
    assert.commandWorked(coll.insert({a: 1}, {writeConcern: {w: 3}}));
}

// Prepare a regular user without the 'bypassDefaultMaxTimeMS' privilege.
adminDB.createUser({user: 'regularUser', pwd: 'password', roles: ["readWriteAnyDatabase"]});

const regularUserConn = new Mongo(primary.host).getDB('admin');
assert(regularUserConn.auth('regularUser', 'password'), "Auth failed");
const regularUserDB = regularUserConn.getSiblingDB(dbName);

// A long running query without maxTimeMS specified will succeed.
assert.commandWorked(
    regularUserDB.runCommand({find: collName, filter: {$where: "sleep(1000); return true;"}}));

// A long running query with a small maxTimeMS specified will fail.
assert.commandFailedWithCode(
    regularUserDB.runCommand(
        {find: collName, filter: {$where: "sleep(1000); return true;"}, maxTimeMS: 1}),
    ErrorCodes.MaxTimeMSExpired);

// Sets the default maxTimeMS for read operations with a small value.
assert.commandWorked(
    adminDB.runCommand({setClusterParameter: {defaultMaxTimeMS: {readOperations: 1}}}));

// The read command fails even without specifying a maxTimeMS option.
assert.commandFailedWithCode(
    regularUserDB.runCommand({find: collName, filter: {$where: "sleep(1000); return true;"}}),
    ErrorCodes.MaxTimeMSExpired);

// The read command will succeed if specifying a large maxTimeMS option. In this case, it's chosen
// over the default value.
assert.commandWorked(regularUserDB.runCommand(
    {find: collName, filter: {$where: "sleep(1000); return true;"}, maxTimeMS: 50000}));

// The default read MaxTimeMS value doesn't affect write commands.
assert.commandWorked(regularUserDB.runCommand(
    {update: collName, updates: [{q: {$where: "sleep(1000); return true;"}, u: {$inc: {a: 1}}}]}));

// Tests the secondaries behave correctly too.
rst.getSecondaries().forEach(secondary => {
    const regularUserConnSecondary = new Mongo(secondary.host);
    regularUserConnSecondary.setSecondaryOk();
    assert(regularUserConnSecondary.getDB('admin').auth('regularUser', 'password'), "Auth failed");
    const regularUserDBSecondary = regularUserConnSecondary.getDB(dbName);
    // The read command fails even without specifying a maxTimeMS option.
    assert.commandFailedWithCode(
        regularUserDBSecondary.runCommand(
            {find: collName, filter: {$where: "sleep(1000); return true;"}}),
        ErrorCodes.MaxTimeMSExpired);

    // The read command will succeed if specifying a large maxTimeMS option. In this case, it's
    // chosen over the default value.
    assert.commandWorked(regularUserDBSecondary.runCommand(
        {find: collName, filter: {$where: "sleep(1000); return true;"}, maxTimeMS: 50000}));
});

// Unsets the default MaxTimeMS to make queries not to time out in the following code.
assert.commandWorked(
    adminDB.runCommand({setClusterParameter: {defaultMaxTimeMS: {readOperations: 0}}}));

adminDB.logout();
regularUserDB.logout();

rst.stopSet();
