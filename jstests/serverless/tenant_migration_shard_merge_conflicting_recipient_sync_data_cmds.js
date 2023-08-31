/**
 * Test that shard merge recipient rejects conflicting recipientSyncData commands.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   requires_fcv_52,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   requires_shard_merge,
 *   serverless,
 *   requires_fcv_71,
 * ]
 */

import {Thread} from "jstests/libs/parallelTester.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {
    kProtocolShardMerge,
} from "jstests/replsets/libs/tenant_migration_util.js";

const kDonorConnectionString0 = "foo/bar:12345";
const kDonorConnectionString1 = "foo/bar:56789";
const kPrimaryReadPreference = {
    mode: "primary"
};

TestData.stopFailPointErrorCode = 4880402;

/**
 * Runs recipientSyncData on the given host and returns the response.
 */
function runRecipientSyncDataCmd(primaryHost,
                                 {
                                     migrationIdString,
                                     tenantIds,
                                     donorConnectionString,
                                     readPreference,
                                 },
                                 protocol) {
    jsTestLog("Starting a recipientSyncDataCmd for migrationId: " + migrationIdString +
              " tenantIds: '" + tenantIds + "'");
    const primary = new Mongo(primaryHost);
    const res = primary.adminCommand({
        recipientSyncData: 1,
        migrationId: UUID(migrationIdString),
        donorConnectionString: donorConnectionString,
        tenantIds: eval(tenantIds),
        protocol,
        readPreference: readPreference,
        startMigrationDonorTimestamp: Timestamp(1, 1),
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
 * Tests that if the client runs multiple recipientSyncData commands that would start conflicting
 * migrations, only one of the migrations will start and succeed.
 */
function testConcurrentConflictingMigration(migrationOpts0, migrationOpts1) {
    var rst = new ReplSetTest({nodes: 1, serverless: true});
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
        new Thread(runRecipientSyncDataCmd, primary.host, migrationOpts0, kProtocolShardMerge);
    const recipientSyncDataThread1 =
        new Thread(runRecipientSyncDataCmd, primary.host, migrationOpts1, kProtocolShardMerge);
    recipientSyncDataThread0.start();
    recipientSyncDataThread1.start();

    const res0 = assert.commandFailed(recipientSyncDataThread0.returnData());
    const res1 = assert.commandFailed(recipientSyncDataThread1.returnData());

    if (res0.code == TestData.stopFailPointErrorCode) {
        assert.commandFailedWithCode(res0, TestData.stopFailPointErrorCode);
        assert.commandFailedWithCode(res1, ErrorCodes.ConflictingOperationInProgress);
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
    };
    const migrationOpts1 = Object.extend({}, migrationOpts0, true);
    migrationOpts1.donorConnectionString = kDonorConnectionString1;
    testConcurrentConflictingMigration(migrationOpts0, migrationOpts1);
})();
