/**
 * Test that shard merge recipient rejects conflicting recipientSyncData commands.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   requires_fcv_52,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   featureFlagShardMerge,
 *   serverless,
 * ]
 */

import {
    getCertificateAndPrivateKey,
    isShardMergeEnabled,
    makeX509OptionsForTest,
} from "jstests/replsets/libs/tenant_migration_util.js";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");
load("jstests/libs/uuid_util.js");

const standalone = MongoRunner.runMongod({});
const shardMergeFeatureFlagEnabled = isShardMergeEnabled(standalone.getDB("admin"));
MongoRunner.stopMongod(standalone);

// Note: including this explicit early return here due to the fact that multiversion
// suites will execute this test without featureFlagShardMerge enabled (despite the
// presence of the featureFlagShardMerge tag above), which means the test will attempt
// to run a multi-tenant migration and fail.
if (!shardMergeFeatureFlagEnabled) {
    jsTestLog("Skipping Shard Merge-specific test");
    quit();
}

const kDonorConnectionString0 = "foo/bar:12345";
const kDonorConnectionString1 = "foo/bar:56789";
const kPrimaryReadPreference = {
    mode: "primary"
};
const kRecipientCertificateForDonor =
    getCertificateAndPrivateKey("jstests/libs/tenant_migration_recipient.pem");
const kExpiredRecipientCertificateForDonor =
    getCertificateAndPrivateKey("jstests/libs/tenant_migration_recipient_expired.pem");

TestData.stopFailPointErrorCode = 4880402;

/**
 * Runs recipientSyncData on the given host and returns the response.
 */
function runRecipientSyncDataCmd(primaryHost, {
    migrationIdString,
    tenantIds,
    donorConnectionString,
    readPreference,
    recipientCertificateForDonor
}) {
    jsTestLog("Starting a recipientSyncDataCmd for migrationId: " + migrationIdString +
              " tenantIds: '" + tenantIds + "'");
    const primary = new Mongo(primaryHost);
    const res = primary.adminCommand({
        recipientSyncData: 1,
        migrationId: UUID(migrationIdString),
        donorConnectionString: donorConnectionString,
        tenantIds: eval(tenantIds),
        protocol: "shard merge",
        readPreference: readPreference,
        startMigrationDonorTimestamp: Timestamp(1, 1),
        recipientCertificateForDonor: recipientCertificateForDonor
    });
    return res;
}

/**
 * Returns an array of currentOp entries for the TenantMigrationRecipientService instances that
 * match the given query.
 */
function getTenantMigrationRecipientCurrentOpEntries(recipientPrimary, query) {
    const cmdObj = Object.assign({currentOp: true, desc: "shard merge recipient"}, query);
    return assert.commandWorked(recipientPrimary.adminCommand(cmdObj)).inprog;
}

/**
 * Asserts that the string does not contain certificate or private pem string.
 */
function assertNoCertificateOrPrivateKey(string) {
    assert(!string.includes("CERTIFICATE"), "found certificate");
    assert(!string.includes("PRIVATE KEY"), "found private key");
}

/**
 * Tests that if the client runs multiple recipientSyncData commands that would start conflicting
 * migrations, only one of the migrations will start and succeed.
 */
function testConcurrentConflictingMigration(migrationOpts0, migrationOpts1) {
    var rst =
        new ReplSetTest({nodes: 1, serverless: true, nodeOptions: makeX509OptionsForTest().donor});
    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();
    const configRecipientsColl = primary.getDB("config")["shardMergeRecipients"];

    // Enable the failpoint to stop the tenant migration after persisting the state doc.
    assert.commandWorked(primary.adminCommand({
        configureFailPoint: "fpAfterPersistingTenantMigrationRecipientInstanceStateDoc",
        mode: "alwaysOn",
        data: {action: "stop", stopErrorCode: NumberInt(TestData.stopFailPointErrorCode)}
    }));

    // Start the conflicting recipientSyncData cmds.
    const recipientSyncDataThread0 =
        new Thread(runRecipientSyncDataCmd, primary.host, migrationOpts0);
    const recipientSyncDataThread1 =
        new Thread(runRecipientSyncDataCmd, primary.host, migrationOpts1);
    recipientSyncDataThread0.start();
    recipientSyncDataThread1.start();

    const res0 = assert.commandFailed(recipientSyncDataThread0.returnData());
    const res1 = assert.commandFailed(recipientSyncDataThread1.returnData());

    if (res0.code == TestData.stopFailPointErrorCode) {
        assert.commandFailedWithCode(res0, TestData.stopFailPointErrorCode);
        assert.commandFailedWithCode(res1, ErrorCodes.ConflictingOperationInProgress);
        assertNoCertificateOrPrivateKey(res1.errmsg);
        assert.eq(1, configRecipientsColl.count({_id: UUID(migrationOpts0.migrationIdString)}));
        assert.eq(1, getTenantMigrationRecipientCurrentOpEntries(primary, {
                         "instanceID": UUID(migrationOpts0.migrationIdString)
                     }).length);
        if (migrationOpts0.migrationIdString != migrationOpts1.migrationIdString) {
            assert.eq(0, configRecipientsColl.count({_id: UUID(migrationOpts1.migrationIdString)}));
            assert.eq(0, getTenantMigrationRecipientCurrentOpEntries(primary, {
                             "instanceID": UUID(migrationOpts1.migrationIdString)
                         }).length);
        } else if (migrationOpts0.tenantIds != migrationOpts1.tenantIds) {
            assert.eq(0, configRecipientsColl.count({tenantIds: eval(migrationOpts1.tenantIds)}));
            assert.eq(0, getTenantMigrationRecipientCurrentOpEntries(primary, {
                             tenantIds: eval(migrationOpts1.tenantIds)
                         }).length);
        }
    } else {
        assert.commandFailedWithCode(res0, ErrorCodes.ConflictingOperationInProgress);
        assert.commandFailedWithCode(res1, TestData.stopFailPointErrorCode);
        assertNoCertificateOrPrivateKey(res0.errmsg);
        assert.eq(1, configRecipientsColl.count({_id: UUID(migrationOpts1.migrationIdString)}));
        assert.eq(1, getTenantMigrationRecipientCurrentOpEntries(primary, {
                         "instanceID": UUID(migrationOpts1.migrationIdString)
                     }).length);
        if (migrationOpts0.migrationIdString != migrationOpts1.migrationIdString) {
            assert.eq(0, configRecipientsColl.count({_id: UUID(migrationOpts0.migrationIdString)}));
            assert.eq(0, getTenantMigrationRecipientCurrentOpEntries(primary, {
                             "instanceID": UUID(migrationOpts0.migrationIdString)
                         }).length);
        } else if (migrationOpts0.tenantIds != migrationOpts1.tenantIds) {
            assert.eq(0, configRecipientsColl.count({tenantId: eval(migrationOpts0.tenantIds)}));
            assert.eq(0, getTenantMigrationRecipientCurrentOpEntries(primary, {
                             tenantIds: eval(migrationOpts0.tenantIds)
                         }).length);
        }
    }

    rst.stopSet();
}

// Test migrations with different migrationIds but identical settings.
(() => {
    const migrationOpts0 = {
        migrationIdString: extractUUIDFromObject(UUID()),
        tenantIds: tojson([ObjectId()]),
        donorConnectionString: kDonorConnectionString0,
        readPreference: kPrimaryReadPreference,
        recipientCertificateForDonor: kRecipientCertificateForDonor
    };
    const migrationOpts1 = Object.extend({}, migrationOpts0, true);
    migrationOpts1.migrationIdString = extractUUIDFromObject(UUID());
    testConcurrentConflictingMigration(migrationOpts0, migrationOpts1);
})();

// Test reusing a migrationId with different migration settings.

// Test different tenantIds.
(() => {
    const migrationOpts0 = {
        migrationIdString: extractUUIDFromObject(UUID()),
        tenantIds: tojson([ObjectId()]),
        donorConnectionString: kDonorConnectionString0,
        readPreference: kPrimaryReadPreference,
        recipientCertificateForDonor: kRecipientCertificateForDonor
    };
    const migrationOpts1 = Object.extend({}, migrationOpts0, true);
    migrationOpts1.tenantIds = tojson([ObjectId()]);
    testConcurrentConflictingMigration(migrationOpts0, migrationOpts1);
})();

// Test different donor connection strings.
(() => {
    const migrationOpts0 = {
        migrationIdString: extractUUIDFromObject(UUID()),
        tenantIds: tojson([ObjectId()]),
        donorConnectionString: kDonorConnectionString0,
        readPreference: kPrimaryReadPreference,
        recipientCertificateForDonor: kRecipientCertificateForDonor
    };
    const migrationOpts1 = Object.extend({}, migrationOpts0, true);
    migrationOpts1.donorConnectionString = kDonorConnectionString1;
    testConcurrentConflictingMigration(migrationOpts0, migrationOpts1);
})();

// Test different certificates.
(() => {
    const migrationOpts0 = {
        migrationIdString: extractUUIDFromObject(UUID()),
        tenantIds: tojson([ObjectId()]),
        donorConnectionString: kDonorConnectionString0,
        readPreference: kPrimaryReadPreference,
        recipientCertificateForDonor: kRecipientCertificateForDonor
    };
    const migrationOpts1 = Object.extend({}, migrationOpts0, true);
    migrationOpts1.recipientCertificateForDonor = kExpiredRecipientCertificateForDonor;
    testConcurrentConflictingMigration(migrationOpts0, migrationOpts1);
})();
