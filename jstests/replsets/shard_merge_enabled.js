/**
 * Tests that the "shard merge" protocol is enabled only in the proper FCV.
 * @tags: [requires_fcv_52, featureFlagShardMerge, disabled_due_to_server_61671]
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

    function recipientForgetMigrationCmd() {
        return {
            recipientForgetMigration: 1,
            protocol: "shard merge",
            migrationId: UUID(),
            donorConnectionString: kDummyConnStr,
            readPreference: readPreference,
            recipientCertificateForDonor: migrationCertificates.recipientCertificateForDonor
        };
    }

    function func(cmd) {
        return eval(cmd + "Cmd()");
    }

    function upgradeTest(cmd) {
        let msg = cmd + " shouldn't reject 'shard merge' protocol when it's enabled";
        assert.commandWorked(adminDB.runCommand(func(cmd)), msg);
    }

    function downgradeTest(cmd) {
        let msg = cmd + " should reject 'shard merge' protocol when it's disabled";
        let expectedErrorMsg = "'protocol' field is not supported for FCV below 5.2'";
        let response = assert.commandFailedWithCode(
            adminDB.runCommand(func(cmd)), ErrorCodes.InvalidOptions, msg);
        assert.neq(-1,
                   response.errmsg.indexOf(expectedErrorMsg),
                   "Error message did not contain '" + expectedErrorMsg + "', found:\n" +
                       tojson(response));
    }

    // Enable below fail points to prevent starting the donor/recipient POS instance.
    configureFailPoint(primary, "returnResponseCommittedForDonorStartMigrationCmd");
    configureFailPoint(primary, "returnResponseOkForRecipientSyncDataCmd");
    configureFailPoint(primary, "returnResponseOkForRecipientForgetMigrationCmd");

    // Preconditions: the shard merge feature is enabled and our fresh RS is on the latest FCV.
    assert(TenantMigrationUtil.isShardMergeEnabled(adminDB));
    assert.eq(getFCVConstants().latest,
              adminDB.system.version.findOne({_id: 'featureCompatibilityVersion'}).version);

    // Shard merge is enabled, so this call should work.
    let cmds = ["donorStartMigration", "recipientSyncData", "recipientForgetMigration"];
    cmds.forEach((cmd) => {
        upgradeTest(cmd);
    });

    assert.commandWorked(adminDB.adminCommand({setFeatureCompatibilityVersion: downgradeFCV}));

    // Now that FCV is downgraded, shard merge is automatically disabled.
    cmds.forEach((cmd) => {
        downgradeTest(cmd);
    });

    rst.stopSet();
}

runFeatureFlagMultiversionTest('featureFlagShardMerge', runTest);
})();
