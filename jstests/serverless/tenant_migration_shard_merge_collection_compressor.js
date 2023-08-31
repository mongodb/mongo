/**
 * Tests that shard merge works with different collection compressor options.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 *   requires_fcv_71,
 *   requires_shard_merge
 * ]
 */

import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";
import {
    makeTenantDB,
    makeX509OptionsForTest,
} from "jstests/replsets/libs/tenant_migration_util.js";

function runTest(nodeCollectionCompressorOptions) {
    jsTestLog("Testing tenant migration for the following collection compressor options: " +
              tojson(nodeCollectionCompressorOptions));

    // Allow donor to run data consistency checks after migration commit.
    const donorSetParamOptions = {
        setParameter:
            {"failpoint.tenantMigrationDonorAllowsNonTimestampedReads": tojson({mode: "alwaysOn"})}
    };
    const donorRst = new ReplSetTest({
        nodes: 1,
        name: 'donorRst',
        serverless: true,
        nodeOptions: Object.assign(
            makeX509OptionsForTest().donor, nodeCollectionCompressorOptions, donorSetParamOptions)
    });
    donorRst.startSet();
    donorRst.initiate();

    const recipientRst = new ReplSetTest({
        nodes: 1,
        name: 'recipientRst',
        serverless: true,
        nodeOptions:
            Object.assign(makeX509OptionsForTest().recipient, nodeCollectionCompressorOptions)
    });
    recipientRst.startSet();
    recipientRst.initiate();

    const tenantMigrationTest = new TenantMigrationTest({donorRst, recipientRst});

    const tenantId = ObjectId();
    const tenantDB = makeTenantDB(tenantId.str, "testDB");
    const collName = "testColl";

    // Do a majority write.
    tenantMigrationTest.insertDonorDB(tenantDB, collName);

    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(UUID()),
        tenantIds: [tenantId],
    };

    TenantMigrationTest.assertCommitted(tenantMigrationTest.runMigration(migrationOpts));

    tenantMigrationTest.stop();
    recipientRst.stopSet();
    donorRst.stopSet();
}

["snappy",
 "zlib",
 "zstd"]
    .forEach(option => runTest({"wiredTigerCollectionBlockCompressor": option}));
