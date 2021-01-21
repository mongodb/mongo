/**
 * Verify that after a tenant migration the donor can validate the recipient's cluster times.
 *
 * @tags: [requires_fcv_47, requires_majority_read_concern, incompatible_with_eft]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");

// Multiple users cannot be authenticated on one connection within a session.
TestData.disableImplicitSessions = true;

// User that runs the tenant migration.
const kAdminUser = {
    name: "admin",
    pwd: "pwd",
};

// User that runs commands against the tenant database.
const kTestUser = {
    name: "testUser",
    pwd: "pwd",
};

function createUsers(primary) {
    const adminDB = primary.getDB("admin");
    assert.commandWorked(
        adminDB.runCommand({createUser: kAdminUser.name, pwd: kAdminUser.pwd, roles: ["root"]}));
    assert.eq(1, adminDB.auth(kAdminUser.name, kAdminUser.pwd));

    const testDB = primary.getDB(kDbName);
    assert.commandWorked(
        testDB.runCommand({createUser: kTestUser.name, pwd: kTestUser.pwd, roles: ["readWrite"]}));

    adminDB.logout();
}

const kTenantId = "testTenantId";
const kDbName = kTenantId + "_" +
    "testDb";
const kCollName = "testColl";

const x509Options = TenantMigrationUtil.makeX509OptionsForTest();
const donorRst = new ReplSetTest({
    nodes: 1,
    name: "donor",
    keyFile: "jstests/libs/key1",
    nodeOptions: Object.assign(
        x509Options.donor,
        {setParameter: {"failpoint.alwaysValidateClientsClusterTime": tojson({mode: "alwaysOn"})}}),
});

const recipientRst = new ReplSetTest({
    nodes: 1,
    name: "recipient",
    keyFile: "jstests/libs/key1",
    nodeOptions: Object.assign(
        x509Options.recipient,
        {setParameter: {"failpoint.alwaysValidateClientsClusterTime": tojson({mode: "alwaysOn"})}}),
});

donorRst.startSet();
donorRst.initiate();

recipientRst.startSet();
recipientRst.initiate();

const donorPrimary = donorRst.getPrimary();
const recipientPrimary = recipientRst.getPrimary();

const donorAdminDB = donorPrimary.getDB("admin");
const recipientAdminDB = recipientPrimary.getDB("admin");

const donorTestDB = donorPrimary.getDB(kDbName);
const recipientTestDB = recipientPrimary.getDB(kDbName);

createUsers(donorPrimary);
createUsers(recipientPrimary);

assert.eq(1, donorAdminDB.auth(kAdminUser.name, kAdminUser.pwd));
assert.eq(1, recipientAdminDB.auth(kAdminUser.name, kAdminUser.pwd));

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName(), donorRst, recipientRst});
if (!tenantMigrationTest.isFeatureFlagEnabled()) {
    jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
    donorRst.stopSet();
    recipientRst.stopSet();
    return;
}

donorAdminDB.logout();
recipientAdminDB.logout();

assert.eq(1, donorTestDB.auth(kTestUser.name, kTestUser.pwd));
assert.eq(1, recipientTestDB.auth(kTestUser.name, kTestUser.pwd));

const donorClusterTime =
    assert.commandWorked(donorTestDB.runCommand({find: kCollName})).$clusterTime;
const recipientClusterTime =
    assert.commandWorked(recipientTestDB.runCommand({find: kCollName})).$clusterTime;
jsTest.log("donor's clusterTime " + tojson(donorClusterTime));
jsTest.log("recipient's clusterTime " + tojson(recipientClusterTime));

jsTest.log("Verify that prior to the migration, the donor and recipient fail to validate each " +
           "other's clusterTime");

assert.commandFailedWithCode(
    donorTestDB.runCommand({find: kCollName, $clusterTime: recipientClusterTime}),
    [ErrorCodes.TimeProofMismatch, ErrorCodes.KeyNotFound]);
assert.commandFailedWithCode(
    recipientTestDB.runCommand({find: kCollName, $clusterTime: donorClusterTime}),
    [ErrorCodes.TimeProofMismatch, ErrorCodes.KeyNotFound]);

donorTestDB.logout();
recipientTestDB.logout();

assert.eq(1, donorAdminDB.auth(kAdminUser.name, kAdminUser.pwd));
assert.eq(1, recipientAdminDB.auth(kAdminUser.name, kAdminUser.pwd));

const migrationId = UUID();
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(migrationId),
    tenantId: kTenantId,
};
assert.commandWorked(tenantMigrationTest.runMigration(migrationOpts));

donorAdminDB.logout();
recipientAdminDB.logout();

jsTest.log("Verify that after the migration, the donor can validate the recipient's clusterTime");

assert.eq(1, donorTestDB.auth(kTestUser.name, kTestUser.pwd));
assert.eq(1, recipientTestDB.auth(kTestUser.name, kTestUser.pwd));

assert.commandWorked(donorTestDB.runCommand({find: kCollName, $clusterTime: recipientClusterTime}));
// TODO (SERVER-53405): Test that the recipient can validate the donor's clusterTime.
assert.commandFailedWithCode(
    recipientTestDB.runCommand({find: kCollName, $clusterTime: donorClusterTime}),
    [ErrorCodes.TimeProofMismatch, ErrorCodes.KeyNotFound]);

tenantMigrationTest.stop();
donorRst.stopSet();
recipientRst.stopSet();
})();
