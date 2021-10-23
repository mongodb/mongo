/**
 * Tests that the "shard merge" protocol is enabled only in the proper FCV.
 * @tags: [requires_fcv_51, featureFlagShardMerge]
 */

(function() {
"use strict";

load("jstests/replsets/libs/tenant_migration_util.js");
load("jstests/libs/fail_point_util.js");

function runTest(downgradeFCV) {
    let rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();

    let primary = rst.getPrimary();
    let adminDB = primary.getDB("admin");
    const kDummyConnStr = "mongodb://localhost/?replicaSet=foo";
    const readPreference = {mode: 'primary'};
    const migrationCertificates = TenantMigrationUtil.makeMigrationCertificatesForTest();

    // A function, not a constant, to ensure unique UUIDs.
    function donorStartMigrationCmd() {
        return {
            donorStartMigration: 1,
            protocol: "shard merge",
            migrationId: UUID(),
            recipientConnectionString: kDummyConnStr,
            readPreference: readPreference,
            donorCertificateForRecipient: migrationCertificates.donorCertificateForRecipient,
            recipientCertificateForDonor: migrationCertificates.recipientCertificateForDonor
        };
    }

    function recipientSyncDataCmd() {
        return {
            recipientSyncData: 1,
            protocol: "shard merge",
            migrationId: UUID(),
            donorConnectionString: kDummyConnStr,
            readPreference: readPreference,
            startMigrationDonorTimestamp: Timestamp(1, 1),
            recipientCertificateForDonor: migrationCertificates.recipientCertificateForDonor
        };
    }

    // Enable below fail points to prevent starting the donor/recipient POS instance.
    configureFailPoint(primary, "returnResponseCommittedForDonorStartMigrationCmd");
    configureFailPoint(primary, "returnResponseOkForRecipientSyncDataCmd");

    // Preconditions: the shard merge feature is enabled and our fresh RS is on the latest FCV.
    assert(TenantMigrationUtil.isShardMergeEnabled(adminDB));
    assert.eq(getFCVConstants().latest,
              adminDB.system.version.findOne({_id: 'featureCompatibilityVersion'}).version);

    // Shard merge is enabled, so this call should work.
    assert.commandWorked(
        adminDB.runCommand(donorStartMigrationCmd()),
        "donorStartMigration shouldn't reject 'shard merge' protocol when it's enabled");
    assert.commandWorked(
        adminDB.runCommand(recipientSyncDataCmd()),
        "recipientSyncDataCmd shouldn't reject 'shard merge' protocol when it's enabled");

    assert.commandWorked(adminDB.adminCommand({setFeatureCompatibilityVersion: downgradeFCV}));

    // Now that FCV is downgraded, shard merge is automatically disabled.
    assert.commandFailedWithCode(
        adminDB.runCommand(donorStartMigrationCmd()),
        ErrorCodes.IllegalOperation,
        "donorStartMigration should reject 'shard merge' protocol when it's disabled");
    assert.commandFailedWithCode(
        adminDB.runCommand(recipientSyncDataCmd()),
        ErrorCodes.IllegalOperation,
        "recipientSyncDataCmd should reject 'shard merge' protocol when it's disabled");

    rst.stopSet();
}

runFeatureFlagMultiversionTest('featureFlagShardMerge', runTest);
})();
