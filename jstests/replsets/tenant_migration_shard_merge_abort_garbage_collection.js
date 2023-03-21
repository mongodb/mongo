/**
 * Tests that shard merge deletes migrated tenant data if it aborts.
 *
 * @tags: [
 *   featureFlagShardMerge,
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 * ]
 */

import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";
import {
    forgetMigrationAsync,
    isShardMergeEnabled
} from "jstests/replsets/libs/tenant_migration_util.js";
load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/rslib.js");  // `createRstArgs`
load("jstests/serverless/libs/change_collection_util.js");

// Disabling featureFlagRequireTenantID to allow using a tenantId prefix (instead of $tenant) and
// reusing the same code to test garbage collection with and without multitenancy support.
delete TestData.setParameters.featureFlagRequireTenantID;

function collectionExists(node, dbName, collName, tenant) {
    const res =
        assert.commandWorked(node.getDB(tenant.str + "_" + dbName)
                                 .runCommand({listCollections: 1, filter: {name: collName}}));
    return res.cursor.firstBatch.length == 1;
}

function getDatabasesForTenant(node) {
    return assert.commandWorked(node.getDB("admin").runCommand(
        {listDatabases: 1, nameOnly: true, filter: {"name": /^[0-9a-z]+_db[0-9]/}}));
}

function assertDatabasesDroppedForTenant(node) {
    const dbs = getDatabasesForTenant(node);
    assert.eq(0, dbs.databases.length);
}

function loadDummyData() {
    const numDocs = 20;
    const testData = [];
    for (let i = 0; i < numDocs; ++i) {
        testData.push({_id: i, x: i});
    }
    return testData;
}

function runTest({multitenancySupport}) {
    const setParameter = {
        tenantMigrationGarbageCollectionDelayMS: 0,
        multitenancySupport,

    };

    const tenantMigrationTest =
        new TenantMigrationTest({name: jsTestName(), sharedOptions: {setParameter}});
    const tenantId = ObjectId();

    // Note: including this explicit early return here due to the fact that multiversion
    // suites will execute this test without featureFlagShardMerge enabled (despite the
    // presence of the featureFlagShardMerge tag above), which means the test will attempt
    // to run a multi-tenant migration and fail.
    if (!isShardMergeEnabled(tenantMigrationTest.getRecipientPrimary().getDB("admin"))) {
        tenantMigrationTest.stop();
        jsTestLog("Skipping Shard Merge-specific test");
        quit();
    }

    configureFailPoint(tenantMigrationTest.getDonorPrimary(),
                       "abortTenantMigrationBeforeLeavingBlockingState");
    const fp =
        configureFailPoint(tenantMigrationTest.getRecipientPrimary(),
                           "pauseTenantMigrationBeforeMarkingExternalKeysGarbageCollectable");
    const dataSyncFp = configureFailPoint(tenantMigrationTest.getDonorPrimary(),
                                          "pauseTenantMigrationBeforeLeavingDataSyncState");

    const dbNamesNoPrefix = ["db0", "db1", "db2"];
    const dbNames = dbNamesNoPrefix.map(entry => tenantId.str + "_" + entry);
    const collNames = ["coll0", "coll1"];

    for (const dbName of [...dbNames]) {
        for (const coll of collNames) {
            const db = tenantMigrationTest.getDonorPrimary().getDB(dbName);
            assert.commandWorked(db.runCommand(
                {insert: coll, documents: loadDummyData(), writeConcern: {w: 'majority'}}));
            const viewCollName = coll + "GreaterThanView";
            assert.commandWorked(db.runCommand(
                {create: viewCollName, viewOn: coll, pipeline: [{$match: {x: {$gt: 8}}}]}));
            const indexName = coll + "IndexOnX";
            assert.commandWorked(
                db.runCommand({createIndexes: coll, indexes: [{key: {x: 1}, name: indexName}]}));
        }
    }

    const tsDB = tenantId.str + "_db4";
    const tsColl = "tsColl";
    const db = tenantMigrationTest.getDonorPrimary().getDB(tsDB);
    assert.commandWorked(db.runCommand({create: tsDB, timeseries: {timeField: "ts"}}));
    const testData = [];
    for (let i = 0; i < 20; ++i) {
        testData.push({ts: new Date(i * 1000), x: i});
    }
    assert.commandWorked(
        db.runCommand({insert: tsDB, documents: testData, writeConcern: {w: 'majority'}}));

    assert.neq(getDatabasesForTenant(tenantMigrationTest.getDonorPrimary()).databases.length, 0);

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: tenantId.str,
    };

    assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));

    // Insert collection during oplog catchup.
    dataSyncFp.wait();
    const dataSyncDB = tenantId.str + "_db5";
    const dataSyncColl = "baz";
    assert.commandWorked(tenantMigrationTest.getDonorPrimary().getDB(dataSyncDB).runCommand({
        insert: dataSyncColl,
        documents: loadDummyData(),
        writeConcern: {w: 'majority'}
    }));
    dataSyncFp.off();

    TenantMigrationTest.assertAborted(tenantMigrationTest.waitForMigrationToComplete(
        migrationOpts, false /* retryOnRetryableErrors */, false /* forgetMigration */));

    const forgetMigrationThread = new Thread(forgetMigrationAsync,
                                             migrationOpts.migrationIdString,
                                             createRstArgs(tenantMigrationTest.getDonorRst()),
                                             false /* retryOnRetryableErrors */);

    forgetMigrationThread.start();
    fp.wait();

    for (const node of tenantMigrationTest.getRecipientRst().nodes) {
        for (const dbName of [...dbNames]) {
            for (const coll of collNames) {
                collectionExists(node, dbName, coll, tenantId);
            }
        }
        collectionExists(node, dataSyncDB, dataSyncColl, tenantId);
        collectionExists(node, tsDB, tsColl, tenantId);
    }

    fp.off();
    forgetMigrationThread.join();

    for (var node of tenantMigrationTest.getRecipientRst().nodes) {
        assertDatabasesDroppedForTenant(node, tenantId);
    }

    tenantMigrationTest.stop();
}

runTest({multitenancySupport: true});

runTest({multitenancySupport: false});
