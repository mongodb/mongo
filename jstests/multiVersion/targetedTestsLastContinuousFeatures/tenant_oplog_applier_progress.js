/**
 * Tests that in a tenant migration, the recipient primary will resume oplog application on
 * failover with the same behavior regardless of the binary version running on the primary
 * vs. secondary.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 * ]
 */

import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";
import {
    checkTenantDBHashes,
    makeX509OptionsForTest,
    runMigrationAsync,
} from "jstests/replsets/libs/tenant_migration_util.js";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");       // for 'extractUUIDFromObject'
load("jstests/libs/parallelTester.js");  // for 'Thread'
load('jstests/multiVersion/libs/multi_rs.js');
load('jstests/replsets/rslib.js');  // For 'createRstArgs'

const docsToApply = [{_id: 2, x: 2}, {_id: 3, x: 3}, {_id: 4, x: 4}];

function runTest({initialPrimaryVersion, initialSecondaryVersion, featureCompatibilityVersion}) {
    jsTestLog(`Test TenantOplogApplier failover with Primary version '${
        initialPrimaryVersion}' Secondary version: ${initialSecondaryVersion} and FCV '${
        featureCompatibilityVersion}'`);

    const recipientRst = new ReplSetTest({
        name: jsTestName(),
        nodes: [{binVersion: initialPrimaryVersion}, {binVersion: initialSecondaryVersion}],
        // Use a batch size of 2 so that we can hang in the middle of tenant oplog application.
        nodeOptions: Object.assign(makeX509OptionsForTest().recipient,
                                   {setParameter: {tenantApplierBatchSizeOps: 2}})
    });

    recipientRst.startSet();
    recipientRst.initiate();

    const tenantMigrationTest = new TenantMigrationTest({name: jsTestName(), recipientRst});

    const donorPrimary = tenantMigrationTest.getDonorPrimary();
    const recipientPrimary = tenantMigrationTest.getRecipientPrimary();

    jsTestLog(`Set Donor FCV to ${featureCompatibilityVersion}`);
    assert.commandWorked(
        donorPrimary.adminCommand({setFeatureCompatibilityVersion: featureCompatibilityVersion}));

    jsTestLog(`Set Recipient FCV to ${featureCompatibilityVersion}`);
    assert.commandWorked(recipientPrimary.adminCommand(
        {setFeatureCompatibilityVersion: featureCompatibilityVersion}));

    const tenantId = ObjectId().str;
    const dbName = tenantMigrationTest.tenantDB(tenantId, "testDB");
    const collName = "testColl";
    const donorTestColl = donorPrimary.getDB(dbName).getCollection(collName);

    // Populate the donor replica set with some initial data and make sure it is majority committed.
    const majorityCommittedDocs = [{_id: 0, x: 0}, {_id: 1, x: 1}];
    assert.commandWorked(
        donorTestColl.insert(majorityCommittedDocs, {writeConcern: {w: "majority"}}));
    assert.eq(2, donorTestColl.find().readConcern("majority").itcount());

    const migrationId = UUID();

    const fetchNoOps = () => {
        const recipientPrimary = recipientRst.getPrimary();
        const local = recipientPrimary.getDB("local");
        return local.oplog.rs.find({fromTenantMigration: migrationId, op: "n"}).toArray();
    };

    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        recipientConnString: tenantMigrationTest.getRecipientConnString(),
        protocol: "multitenant migrations",
        tenantId,
    };

    // Configure fail point to have the recipient primary hang after the cloner completes and the
    // oplog applier has started.
    const waitAfterDatabaseClone =
        configureFailPoint(recipientPrimary,
                           "fpAfterStartingOplogApplierMigrationRecipientInstance",
                           {action: "hang"});
    // Configure fail point to hang the tenant oplog applier after it applies the first batch.
    const waitInOplogApplier = configureFailPoint(recipientPrimary, "hangInTenantOplogApplication");

    // Start a migration and wait for recipient to hang in the tenant database cloner.
    const migrationThread = new Thread(
        runMigrationAsync, migrationOpts, createRstArgs(tenantMigrationTest.getDonorRst()));
    migrationThread.start();
    waitAfterDatabaseClone.wait();

    // Insert some writes that will eventually be picked up by the tenant oplog applier on the
    // recipient.
    tenantMigrationTest.insertDonorDB(dbName, collName, docsToApply);

    // Wait for the applied oplog batch to be replicated.
    waitInOplogApplier.wait();
    recipientRst.awaitReplication();

    // It is possible that the first batch applied includes a resume no-op token. We do not write
    // no-op entries for resume token entries in tenant migrations.
    const results = fetchNoOps();
    assert.gt(results.length, 0, results);
    assert.lte(results.length, 2, results);
    assert.eq(docsToApply[0], results[0].o2.o, results);
    if (results.length === 2) {
        assert.eq(docsToApply[1], results[1].o2.o, results);
    }

    jsTestLog(`Step up a new Primary with version ${initialSecondaryVersion}`);
    // Step up a new node in the recipient set and trigger a failover. The new primary should resume
    // fetching starting from the unapplied documents.
    recipientRst.stepUp(recipientRst.getSecondaries()[0]);
    waitAfterDatabaseClone.off();
    waitInOplogApplier.off();

    // The migration should go through after recipient failover.
    TenantMigrationTest.assertCommitted(migrationThread.returnData());

    const appliedNoOps = fetchNoOps();
    assert.eq(3, appliedNoOps.length, appliedNoOps);
    docsToApply.forEach((docToApply, idx) => {
        assert.eq(docToApply, appliedNoOps[idx].o2.o, appliedNoOps);
    });

    checkTenantDBHashes({
        donorRst: tenantMigrationTest.getDonorRst(),
        recipientRst: tenantMigrationTest.getRecipientRst(),
        tenantId
    });

    tenantMigrationTest.stop();
    recipientRst.stopSet();
}

// Initial primary is running 6.3, use FCV 6.3 and fall back to oplog scanning for tenant oplog
// applier resumption.
runTest({
    initialPrimaryVersion: "last-continuous",
    initialSecondaryVersion: "latest",
    featureCompatibilityVersion: "6.3"
});

// Initial secondary is running 6.3, use FCV 6.3 and fall back to oplog scanning for tenant oplog
// applier resumption.
runTest({
    initialPrimaryVersion: "latest",
    initialSecondaryVersion: "last-continuous",
    featureCompatibilityVersion: "6.3"
});

// All nodes running latest version, use latest FCV to utilize progress collection for tenant oplog
// applier resumption.
runTest({
    initialPrimaryVersion: "latest",
    initialSecondaryVersion: "latest",
    featureCompatibilityVersion: "7.0"
});
