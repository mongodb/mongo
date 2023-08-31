/**
 * Tests that the donorStartMigration and recipientSyncData commands throw when a node has options
 * set that are incompatible with protocol shard merge.
 *
 * @tags: [
 *   requires_shard_merge,
 *   serverless,
 *   requires_fcv_71,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {
    kProtocolShardMerge,
} from "jstests/replsets/libs/tenant_migration_util.js";

function runTest(nodeOptions) {
    const rst = new ReplSetTest({nodes: 1, serverless: true, nodeOptions: nodeOptions});
    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();
    const adminDB = primary.getDB("admin");
    const kDummyConnStr = "mongodb://localhost/?replicaSet=foo";
    const readPreference = {mode: 'primary'};

    // Enable below fail points to prevent starting the donor/recipient POS instance.
    configureFailPoint(primary, "returnResponseCommittedForDonorStartMigrationCmd");
    configureFailPoint(primary, "returnResponseOkForRecipientSyncDataCmd");
    configureFailPoint(primary, "returnResponseOkForRecipientForgetMigrationCmd");

    // Ensure the feature flag is enabled and FCV is latest
    assert.eq(getFCVConstants().latest,
              adminDB.system.version.findOne({_id: 'featureCompatibilityVersion'}).version);

    // Assert that donorStartMigration fails with the expected code
    assert.commandFailedWithCode(
        adminDB.runCommand({
            donorStartMigration: 1,
            protocol: kProtocolShardMerge,
            migrationId: UUID(),
            recipientConnectionString: kDummyConnStr,
            readPreference: readPreference,
            tenantIds: [ObjectId()]
        }),
        ErrorCodes.InvalidOptions,
        "Expected donorStartMigration to fail when protocol is 'shard merge' and node options " +
            tojson(nodeOptions) + " are set, but did not");

    // Assert that donorStartMigration fails with the expected code when tenantIds is missing.
    assert.commandFailedWithCode(
        adminDB.runCommand({
            donorStartMigration: 1,
            protocol: kProtocolShardMerge,
            migrationId: UUID(),
            recipientConnectionString: kDummyConnStr,
            readPreference: readPreference,
        }),
        ErrorCodes.InvalidOptions,
        "Expected donorStartMigration to fail when protocol is 'shard merge' and node options " +
            tojson(nodeOptions) + " are set, but did not");

    // Assert that donorStartMigration fails with the expected code when tenantId is provided.
    assert.commandFailedWithCode(
        adminDB.runCommand({
            donorStartMigration: 1,
            protocol: kProtocolShardMerge,
            migrationId: UUID(),
            recipientConnectionString: kDummyConnStr,
            readPreference: readPreference,
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
            protocol: kProtocolShardMerge,
            migrationId: UUID(),
            recipientConnectionString: kDummyConnStr,
            readPreference: readPreference,
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
            protocol: kProtocolShardMerge,
            migrationId: UUID(),
            recipientConnectionString: kDummyConnStr,
            readPreference: readPreference,
            tenantIds: [{}, ObjectId()]
        }),
        ErrorCodes.BadValue,
        "Expected donorStartMigration to fail when protocol is 'shard merge' and node options " +
            tojson(nodeOptions) + " are set, but did not");

    // Assert that donorStartMigration fails with the expected code when tenantIds is empty.
    assert.commandFailedWithCode(
        adminDB.runCommand({
            donorStartMigration: 1,
            protocol: kProtocolShardMerge,
            migrationId: UUID(),
            recipientConnectionString: kDummyConnStr,
            readPreference: readPreference,
            tenantIds: []
        }),
        ErrorCodes.InvalidOptions,
        "Expected donorStartMigration to fail when protocol is 'shard merge' and node options " +
            tojson(nodeOptions) + " are set, but did not");

    // Assert that recipientSyncData fails with the expected code
    assert.commandFailedWithCode(
        adminDB.runCommand({
            recipientSyncData: 1,
            protocol: kProtocolShardMerge,
            migrationId: UUID(),
            tenantIds: [ObjectId()],
            donorConnectionString: kDummyConnStr,
            readPreference: readPreference,
            startMigrationDonorTimestamp: Timestamp(1, 1),
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
