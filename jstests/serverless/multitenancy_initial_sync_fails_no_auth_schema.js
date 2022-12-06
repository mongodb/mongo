/**
 * This test checks that initial sync fails when an auth schema doc does not exist in the global
 * admin database, but a user exists in a tenant's user collection.
 */

(function() {
"use strict";

load("jstests/replsets/rslib.js");  // For reInitiateWithoutThrowingOnAbortedMember

const rst = new ReplSetTest({
    nodes: 1,
    nodeOptions:
        {auth: '', setParameter: {multitenancySupport: true, featureFlagRequireTenantID: true}}
});
rst.startSet({keyFile: 'jstests/libs/key1'});
rst.initiate();

const primary = rst.getPrimary();
const kTenant = ObjectId();

// Authenticate as the __system user so we can delete the auth schema doc.
const adminDb = primary.getDB('admin');
assert.commandWorked(
    adminDb.runCommand({createUser: 'internalUser', pwd: 'pwd', roles: ['__system']}));
assert(adminDb.auth('internalUser', 'pwd'));

// Create a tenant user.
assert.commandWorked(primary.getDB('$external').runCommand({
    createUser: "userTenant1",
    '$tenant': kTenant,
    roles: [{role: 'dbAdminAnyDatabase', db: 'admin'}, {role: 'readWriteAnyDatabase', db: 'admin'}]
}));

// Check we see a user doc in the tenant's admin.system.user collection.
let res =
    assert.commandWorked(adminDb.runCommand({find: "system.users", filter: {}, $tenant: kTenant}));
assert.eq(1, res.cursor.firstBatch.length);

// Delete the auth schema doc. This should cause initial sync to fail, because a user exists
// without an auth schema doc.
res = assert.commandWorked(adminDb.runCommand(
    {delete: "system.version", deletes: [{q: {"_id": "authSchema"}, limit: 1}]}));
assert.eq(1, res.n);

// Attempt to add a secondary to the replica set - initial sync should fail.
const secondary = rst.add({
    setParameter:
        {multitenancySupport: true, featureFlagRequireTenantID: true, numInitialSyncAttempts: 1}
});

const secondaryAdminDB = secondary.getDB("admin");
reInitiateWithoutThrowingOnAbortedMember(rst);

assert.soon(
    function() {
        try {
            secondaryAdminDB.runCommand({ping: 1});
        } catch (e) {
            return true;
        }
        return false;
    },
    "Node should have terminated due to unsupported auth schema during initial sync, but didn't",
    60 * 1000);

rst.stop(secondary, undefined, {allowedExitCode: MongoRunner.EXIT_ABRUPT});
rst.stopSet();
})();
