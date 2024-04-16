/**
 * Tests that operations killed due to 'defaultMaxTimeMS' and the user-specified 'maxTimeMS' options
 * will be recorded in the serverStatus metric accordingly.
 *
 * @tags: [
 *   requires_replication,
 *   requires_auth,
 *   # Transactions aborted upon fcv upgrade or downgrade; cluster parameters use internal txns.
 *   uses_transactions,
 *   featureFlagDefaultReadMaxTimeMS,
 * ]
 */

const rst = new ReplSetTest({
    nodes: 1,
    keyFile: "jstests/libs/key1",
});
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
    assert.commandWorked(coll.insert({a: 1}));
}

// Prepare a regular user without the 'bypassDefaultMaxTimeMS' privilege.
adminDB.createUser({user: 'regularUser', pwd: 'password', roles: ["readWriteAnyDatabase"]});

const regularUserConn = new Mongo(primary.host).getDB('admin');
assert(regularUserConn.auth('regularUser', 'password'), "Auth failed");
const regularUserDB = regularUserConn.getSiblingDB(dbName);

// Sets the default maxTimeMS for read operations with a small value.
assert.commandWorked(
    adminDB.runCommand({setClusterParameter: {defaultMaxTimeMS: {readOperations: 1}}}));

function assertCommandFailedWithMaxTimeMSExpired(
    {cmd, killedDueToMaxTimeMSExpired, killedDueToDefaultMaxTimeMSExpired}) {
    if (killedDueToMaxTimeMSExpired) {
        const maxTimeMSCounter =
            adminDB.runCommand({serverStatus: 1}).metrics.operation.killedDueToMaxTimeMSExpired;
        assert.commandFailedWithCode(regularUserDB.runCommand(cmd), ErrorCodes.MaxTimeMSExpired);
        assert.gt(
            adminDB.runCommand({serverStatus: 1}).metrics.operation.killedDueToMaxTimeMSExpired,
            maxTimeMSCounter);
    }
    if (killedDueToDefaultMaxTimeMSExpired) {
        const maxTimeMSCounter = adminDB.runCommand({serverStatus: 1})
                                     .metrics.operation.killedDueToDefaultMaxTimeMSExpired;
        assert.commandFailedWithCode(regularUserDB.runCommand(cmd), ErrorCodes.MaxTimeMSExpired);
        assert.gt(adminDB.runCommand({serverStatus: 1})
                      .metrics.operation.killedDueToDefaultMaxTimeMSExpired,
                  maxTimeMSCounter);
    }
}

// Times out due to the default value.
assertCommandFailedWithMaxTimeMSExpired({
    cmd: {find: collName, filter: {$where: "sleep(1000); return true;"}},
    killedDueToDefaultMaxTimeMSExpired: true,
});

// Times out due to the request value.
assertCommandFailedWithMaxTimeMSExpired({
    cmd: {find: collName, filter: {$where: "sleep(1000); return true;"}, maxTimeMS: 100},
    killedDueToMaxTimeMSExpired: true,
});

// Times out due to the request value that is equal to the default value.
assertCommandFailedWithMaxTimeMSExpired({
    cmd: {find: collName, filter: {$where: "sleep(1000); return true;"}, maxTimeMS: 1},
    killedDueToMaxTimeMSExpired: true,
});

adminDB.logout();
regularUserDB.logout();

rst.stopSet();
