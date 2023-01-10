/**
 * Tests that the donorStartMigration and recipientSyncData commands throw when a node has options
 * set that are incompatible with protocol shard merge.
 *
 * @tags: [featureFlagShardMerge]
 * ]
 */

import {
    isShardMergeEnabled,
    makeMigrationCertificatesForTest
} from "jstests/replsets/libs/tenant_migration_util.js";
load("jstests/libs/fail_point_util.js");

function runTest(nodeOptions) {
    const rst = new ReplSetTest({nodes: 1, nodeOptions: nodeOptions});
    rst.startSet();
    rst.initiate();

    // Note: including this explicit early return here due to the fact that multiversion
    // suites will execute this test without featureFlagShardMerge enabled (despite the
    // presence of the featureFlagShardMerge tag above), which means the test will attempt
    // to run a multi-tenant migration and fail.
    if (!isShardMergeEnabled(rst.getPrimary().getDB("admin"))) {
        rst.stopSet();
        jsTestLog("Skipping Shard Merge-specific test");
        return;
    }

    const primary = rst.getPrimary();
    const adminDB = primary.getDB("admin");
    const kDummyConnStr = "mongodb://localhost/?replicaSet=foo";
    const readPreference = {mode: 'primary'};
    const migrationCertificates = makeMigrationCertificatesForTest();

    // Enable below fail points to prevent starting the donor/recipient POS instance.
    configureFailPoint(primary, "returnResponseCommittedForDonorStartMigrationCmd");
    configureFailPoint(primary, "returnResponseOkForRecipientSyncDataCmd");
    configureFailPoint(primary, "returnResponseOkForRecipientForgetMigrationCmd");

    // Ensure the feature flag is enabled and FCV is latest
    assert(isShardMergeEnabled(adminDB));
    assert.eq(getFCVConstants().latest,
              adminDB.system.version.findOne({_id: 'featureCompatibilityVersion'}).version);

    // Assert that donorStartMigration fails with the expected code
    assert.commandFailedWithCode(
        adminDB.runCommand({
            donorStartMigration: 1,
            protocol: "shard merge",
            migrationId: UUID(),
            recipientConnectionString: kDummyConnStr,
            readPreference: readPreference,
            donorCertificateForRecipient: migrationCertificates.donorCertificateForRecipient,
            recipientCertificateForDonor: migrationCertificates.recipientCertificateForDonor,
            tenantIds: [ObjectId()]
        }),
        ErrorCodes.InvalidOptions,
        "Expected donorStartMigration to fail when protocol is 'shard merge' and node options " +
            tojson(nodeOptions) + " are set, but did not");

    // Assert that donorStartMigration fails with the expected code when tenantIds is missing.
    assert.commandFailedWithCode(
        adminDB.runCommand({
            donorStartMigration: 1,
            protocol: "shard merge",
            migrationId: UUID(),
            recipientConnectionString: kDummyConnStr,
            readPreference: readPreference,
            donorCertificateForRecipient: migrationCertificates.donorCertificateForRecipient,
            recipientCertificateForDonor: migrationCertificates.recipientCertificateForDonor
        }),
        ErrorCodes.InvalidOptions,
        "Expected donorStartMigration to fail when protocol is 'shard merge' and node options " +
            tojson(nodeOptions) + " are set, but did not");

    // Assert that donorStartMigration fails with the expected code when tenantId is provided.
    assert.commandFailedWithCode(
        adminDB.runCommand({
            donorStartMigration: 1,
            protocol: "shard merge",
            migrationId: UUID(),
            recipientConnectionString: kDummyConnStr,
            readPreference: readPreference,
            donorCertificateForRecipient: migrationCertificates.donorCertificateForRecipient,
            recipientCertificateForDonor: migrationCertificates.recipientCertificateForDonor,
            tenantIds: [ObjectId()],
            tenantId: ObjectId().str
        }),
        ErrorCodes.InvalidOptions,
        "Expected donorStartMigration to fail when protocol is 'shard merge' and node options " +
            tojson(nodeOptions) + " are set, but did not");

    // Assert that donorStartMigration fails with the expected code when tenantIds is not an
    // ObjectId.
    assert.commandFailedWithCode(
        adminDB.runCommand({
            donorStartMigration: 1,
            protocol: "shard merge",
            migrationId: UUID(),
            recipientConnectionString: kDummyConnStr,
            readPreference: readPreference,
            donorCertificateForRecipient: migrationCertificates.donorCertificateForRecipient,
            recipientCertificateForDonor: migrationCertificates.recipientCertificateForDonor,
            tenantIds: ["admin"]
        }),
        ErrorCodes.BadValue,
        "Expected donorStartMigration to fail when protocol is 'shard merge' and node options " +
            tojson(nodeOptions) + " are set, but did not");

    // Assert that donorStartMigration fails with the expected code when one of the tenantIds is
    // empty.
    assert.commandFailedWithCode(
        adminDB.runCommand({
            donorStartMigration: 1,
            protocol: "shard merge",
            migrationId: UUID(),
            recipientConnectionString: kDummyConnStr,
            readPreference: readPreference,
            donorCertificateForRecipient: migrationCertificates.donorCertificateForRecipient,
            recipientCertificateForDonor: migrationCertificates.recipientCertificateForDonor,
            tenantIds: [{}, ObjectId()]
        }),
        ErrorCodes.BadValue,
        "Expected donorStartMigration to fail when protocol is 'shard merge' and node options " +
            tojson(nodeOptions) + " are set, but did not");

    // Assert that donorStartMigration fails with the expected code when tenantIds is empty.
    assert.commandFailedWithCode(
        adminDB.runCommand({
            donorStartMigration: 1,
            protocol: "shard merge",
            migrationId: UUID(),
            recipientConnectionString: kDummyConnStr,
            readPreference: readPreference,
            donorCertificateForRecipient: migrationCertificates.donorCertificateForRecipient,
            recipientCertificateForDonor: migrationCertificates.recipientCertificateForDonor,
            tenantIds: []
        }),
        ErrorCodes.InvalidOptions,
        "Expected donorStartMigration to fail when protocol is 'shard merge' and node options " +
            tojson(nodeOptions) + " are set, but did not");

    // Assert that recipientSyncData fails with the expected code
    assert.commandFailedWithCode(
        adminDB.runCommand({
            recipientSyncData: 1,
            protocol: "shard merge",
            migrationId: UUID(),
            tenantIds: [ObjectId()],
            donorConnectionString: kDummyConnStr,
            readPreference: readPreference,
            startMigrationDonorTimestamp: Timestamp(1, 1),
            recipientCertificateForDonor: migrationCertificates.recipientCertificateForDonor
        }),
        ErrorCodes.InvalidOptions,
        "Expected recipientSyncData to fail when protocol is 'shard merge' and node options " +
            tojson(nodeOptions) + " are set, but did not");

    rst.stopSet();
}

// Shard merge is not allowed when directoryperdb is enabled
runTest({directoryperdb: ""});

// Shard merge is not allowed when directoryForIndexes is enabled
runTest({"wiredTigerDirectoryForIndexes": ""});
