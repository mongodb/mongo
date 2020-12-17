/**
 * Tests that TenantCollectionCloner completes without error when a collection is dropped during
 * cloning as part of a tenant migration.
 *
 * @tags: [requires_fcv_47, requires_majority_read_concern, incompatible_with_eft]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");
load('jstests/replsets/libs/two_phase_drops.js');

const checkFlagOnly = new TenantMigrationTest({});
const flagEnabled = checkFlagOnly.isFeatureFlagEnabled();
checkFlagOnly.stop();
if (!flagEnabled) {
    jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
    return;
}

function runDropTest({failPointName, failPointData, expectedLog, createNew}) {
    // Configure batch size for recipient clone.
    const recipientRst = new ReplSetTest(
        {nodes: 1, name: "recipient", nodeOptions: {setParameter: {collectionClonerBatchSize: 1}}});

    recipientRst.startSet();
    recipientRst.initiate();

    const tenantMigrationTest =
        new TenantMigrationTest({name: jsTestName(), recipientRst: recipientRst});

    const tenantId = "testTenantId";
    const dbName = tenantMigrationTest.tenantDB(tenantId, "testDB");
    const collName = "testColl";

    const donorPrimary = tenantMigrationTest.getDonorPrimary();
    const recipientPrimary = tenantMigrationTest.getRecipientPrimary();

    // Create a tenant collection with some data in it.
    tenantMigrationTest.insertDonorDB(dbName, collName);

    // Save the nss and uuid for checking log lines.
    const nss = `${dbName}.${collName}`;
    const uuid =
        extractUUIDFromObject(getUUIDFromListCollections(donorPrimary.getDB(dbName), collName));

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        recipientConnString: tenantMigrationTest.getRecipientConnString(),
        tenantId: tenantId,
    };
    const donorRstArgs = TenantMigrationUtil.createRstArgs(tenantMigrationTest.getDonorRst());

    // Set failpoint for recipient.
    const failPoint = configureFailPoint(recipientPrimary, failPointName, failPointData);

    // Start migration and wait for failpoint.
    jsTestLog("Waiting to hit recipient failpoint");
    const migrationThread =
        new Thread(TenantMigrationUtil.runMigrationAsync, migrationOpts, donorRstArgs);
    migrationThread.start();
    failPoint.wait();

    // Drop the collection on the donor while it's being cloned.
    jsTestLog("Dropping collection on donor");
    const donorDB = donorPrimary.getDB(dbName);
    const donorColl = donorDB.getCollection(collName);
    assert(donorColl.drop());
    TwoPhaseDropCollectionTest.waitForDropToComplete(
        donorDB, collName, [{_id: "not the same collection"}]);

    if (createNew) {
        jsTestLog("Creating a new collection with the same name.");
        tenantMigrationTest.insertDonorDB(dbName, collName, [{_id: "not the same collection"}]);
    }

    jsTestLog("Allowing migration to continue");
    failPoint.off();

    if (expectedLog) {
        let expectedLogDict = eval('(' + expectedLog + ')');
        jsTestLog(expectedLogDict);
        checkLog.containsJson(recipientPrimary, expectedLogDict.code, expectedLogDict.attr);
    }

    jsTestLog("Waiting for migration to complete");
    assert.commandWorked(migrationThread.returnData());
    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));

    const recipientColl = recipientPrimary.getDB(dbName).getCollection(collName);

    if (createNew) {
        assert.eq([{_id: "not the same collection"}], recipientColl.find().toArray());
    } else {
        assert.eq(0, recipientColl.find().itcount());
    }

    tenantMigrationTest.stop();
    recipientRst.stopSet();
}

jsTestLog("[1] Testing dropping before count stage.");
runDropTest({
    failPointName: "hangBeforeClonerStage",
    failPointData: {cloner: "TenantCollectionCloner", stage: "count"},
    expectedLog:
        '{code: 5289701, attr: { namespace: nss, uuid: (x)=>(x.uuid.$uuid === uuid), tenantId: tenantId}}'
});

jsTestLog("[2] Testing dropping before count stage, with same-name collection created");
runDropTest({
    failPointName: "hangBeforeClonerStage",
    failPointData: {cloner: "TenantCollectionCloner", stage: "count"},
    expectedLog:
        '{code: 5289701, attr: { namespace: nss, uuid: (x)=>(x.uuid.$uuid === uuid), tenantId: tenantId}}',
    createNew: true
});

jsTestLog("[3] Testing dropping between count and listIndexes.");
runDropTest({
    failPointName: "hangBeforeClonerStage",
    failPointData: {cloner: "TenantCollectionCloner", stage: "listIndexes"},
    expectedLog:
        '{code: 5289701, attr: { namespace: nss, uuid: (x)=>(x.uuid.$uuid === uuid), tenantId: tenantId}}'
});

jsTestLog("[4] Testing dropping between count and listIndexes, with same-name collection created");
runDropTest({
    failPointName: "hangBeforeClonerStage",
    failPointData: {cloner: "TenantCollectionCloner", stage: "listIndexes"},
    expectedLog:
        '{code: 5289701, attr: { namespace: nss, uuid: (x)=>(x.uuid.$uuid === uuid), tenantId: tenantId}}',
    createNew: true
});

jsTestLog("[5] Testing dropping between listIndexes and find.");
runDropTest({
    failPointName: "hangBeforeClonerStage",
    failPointData: {cloner: "TenantCollectionCloner", stage: "query"},
    expectedLog:
        '{code: 5289701, attr: { namespace: nss, uuid: (x)=>(x.uuid.$uuid === uuid), tenantId: tenantId}}'
});

jsTestLog("[6] Testing dropping between listIndexes and find, with same-name collection created");
runDropTest({
    failPointName: "hangBeforeClonerStage",
    failPointData: {cloner: "TenantCollectionCloner", stage: "query"},
    expectedLog:
        '{code: 5289701, attr: { namespace: nss, uuid: (x)=>(x.uuid.$uuid === uuid), tenantId: tenantId}}',
    createNew: true
});

// TODO(SERVER-53282): Enable this test case and add an expectedLog for it.
// jsTestLog("[7] Testing dropping between getMore calls.");
// runDropTest({
//     // Will trigger right after the first batch.
//     failPointName: "tenantMigrationHangCollectionClonerAfterHandlingBatchResponse"
// });

// TODO(SERVER-53282): Enable this test case and add an expectedLog for it.
// jsTestLog("[8] Testing dropping between getMore calls, with same-name collection created");
// runDropTest({
//     // Will trigger right after the first batch.
//     failPointName: "tenantMigrationHangCollectionClonerAfterHandlingBatchResponse",
//     createNew: true
// });
})();