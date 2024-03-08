/**
 * Tests that shard merge deletes migrated tenant data if it aborts.
 *
 * @tags: [
 *   requires_shard_merge,
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 *   requires_fcv_71,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";
import {makeTenantDB} from "jstests/replsets/libs/tenant_migration_util.js";

// Disabling featureFlagRequireTenantID to allow using a tenantId prefix and
// reusing the same code to test garbage collection with and without multitenancy support.
function runTest({multitenancySupport}) {
    const setParameter = {
        tenantMigrationGarbageCollectionDelayMS: 0,
        multitenancySupport,
        featureFlagRequireTenantID: false,
    };

    const tenantMigrationTest =
        new TenantMigrationTest({name: jsTestName(), sharedOptions: {setParameter}});

    const donorPrimary = tenantMigrationTest.getDonorPrimary();
    const recipientPrimary = tenantMigrationTest.getRecipientPrimary();
    const kTenantId = ObjectId().str;

    configureFailPoint(donorPrimary, "abortTenantMigrationBeforeLeavingBlockingState");
    const dataSyncFp =
        configureFailPoint(donorPrimary, "pauseTenantMigrationBeforeLeavingDataSyncState");

    const dbNamesNoPrefix = ["db0", "db1", "db2"];
    const dbNames = dbNamesNoPrefix.map(entry => makeTenantDB(kTenantId, entry));
    const collNames = ["coll0", "coll1"];

    // Create regular collection on donor.
    for (const dbName of [...dbNames]) {
        for (const collName of collNames) {
            tenantMigrationTest.insertDonorDB(dbName, collName);
        }
    }

    // Create view on donor.
    const viewDB = makeTenantDB(kTenantId, "db3");
    const viewCollName = "dummyView";
    assert.commandWorked(donorPrimary.getDB(viewDB).createView(
        viewCollName, "nonExistentCollection", [{$match: {x: {$gte: 8}}}]));
    dbNames.push(viewDB);

    // Create timeseries on donor.
    const tsDB = makeTenantDB(kTenantId, "db4");
    const tsCollName = "tsColl";
    assert.commandWorked(
        donorPrimary.getDB(tsDB).createCollection(tsCollName, {timeseries: {timeField: "ts"}}));
    dbNames.push(tsDB);

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantIds: [ObjectId(kTenantId)],
    };

    // Start migration.
    assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));

    dataSyncFp.wait();

    // Create regular collection on donor during oplog catchup.
    const dataSyncDB = makeTenantDB(kTenantId, "db5");
    const dataSyncColl = "baz";
    tenantMigrationTest.insertDonorDB(dataSyncDB, dataSyncColl);
    dbNames.push(dataSyncDB);

    dataSyncFp.off();

    TenantMigrationTest.assertAborted(tenantMigrationTest.waitForMigrationToComplete(
        migrationOpts, false /* retryOnRetryableErrors */, false /* forgetMigration */));

    // Verify that all tenant collections on donor exists on recipient before forget migration.
    tenantMigrationTest.getRecipientRst().nodes.forEach(node => {
        dbNames.forEach((dbName) => {
            const donorListCollRes =
                assert.commandWorked(donorPrimary.getDB(dbName).runCommand({listCollections: 1}))
                    .cursor.firstBatch;
            const recipientListCollRes =
                assert
                    .commandWorked(recipientPrimary.getDB(dbName).runCommand({listCollections: 1}))
                    .cursor.firstBatch;
            assert(donorListCollRes);
            assert.neq(donorListCollRes.length, 0);
            assert(recipientListCollRes);
            const errorMsg = "Recipient list collections for dbName: " + dbName +
                " not matching with donor:: donorCollectionResult: " + tojson(donorListCollRes) +
                ", recipientCollectionResult: " + tojson(recipientListCollRes);
            assert(bsonBinaryEqual(donorListCollRes, recipientListCollRes), errorMsg);
        });
    });

    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));

    // Forgetting migration should have dropped all donor tenant collections on recipient.
    tenantMigrationTest.getRecipientRst().nodes.forEach(node => {
        dbNames.forEach((dbName) => {
            const recipientListCollRes =
                assert
                    .commandWorked(recipientPrimary.getDB(dbName).runCommand({listCollections: 1}))
                    .cursor.firstBatch;
            assert.eq(recipientListCollRes.length, 0);
        });
    });

    tenantMigrationTest.stop();
}

// TODO SERVER-87536 Re-enable this test when multitenancy is enabled.
// runTest({multitenancySupport: true});

runTest({multitenancySupport: false});
