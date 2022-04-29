/**
 * Test that during tenant migration, multi-key indexes on donor collections can be
 * correctly rebuilt on recipient collections, with the right multi-key paths.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/analyze_plan.js");
load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");

const getQueryExplainIndexScanStage = function(coll) {
    const explain = coll.find().hint({"a.b": 1, "a.c": 1}).explain();
    assert.commandWorked(explain);
    return getPlanStage(getWinningPlan(explain.queryPlanner), "IXSCAN");
};

const verifyMultiKeyIndex = function(coll, isMultiKey, multiKeyPath) {
    const idxScan = getQueryExplainIndexScanStage(coll);
    assert.eq(idxScan.isMultiKey, isMultiKey);
    assert.eq(idxScan.multiKeyPaths, multiKeyPath);
};

const recipientRst = new ReplSetTest({
    nodes: 2,
    name: jsTestName() + "_recipient",
    nodeOptions: Object.assign(TenantMigrationUtil.makeX509OptionsForTest().recipient, {
        setParameter: {
            // Allow reads on recipient before migration completes for testing.
            'failpoint.tenantMigrationRecipientNotRejectReads': tojson({mode: 'alwaysOn'}),
        }
    })
});

recipientRst.startSet();
recipientRst.initiateWithHighElectionTimeout();
const recipientPrimary = recipientRst.getPrimary();

const tenantMigrationTest =
    new TenantMigrationTest({name: jsTestName(), recipientRst: recipientRst});
const donorPrimary = tenantMigrationTest.getDonorPrimary();

const tenantId = "testTenantId";
const dbName = tenantMigrationTest.tenantDB(tenantId, "testDB");

// The first collection on donor side already has the multi-key index.
const collName1 = "multiKeyColl_1";
const donorColl1 = donorPrimary.getDB(dbName).getCollection(collName1);
const recipientColl1 = recipientPrimary.getDB(dbName).getCollection(collName1);
tenantMigrationTest.insertDonorDB(
    dbName,
    collName1,
    [{_id: 0, a: {b: 10, c: 10}}, {_id: 1, a: [{b: [20, 30], c: 40}, {b: 50, c: 60}]}]);
assert.commandWorked(donorColl1.createIndex({"a.b": 1, "a.c": 1}));
// Both "a" and "a.b" are arrays, so the multi-key path on "a.b" is ["a", "a.b"],
// since "a.c" is scala value, the multi-key path on "a.c" only contains "a".
verifyMultiKeyIndex(donorColl1, true, {"a.b": ["a", "a.b"], "a.c": ["a"]});

// The second collection on donor side is not multi-key index yet, during migration,
// new entries will be inserted and it will be turned into multi-key index.
const collName2 = "multiKeyColl_2";
const donorColl2 = donorPrimary.getDB(dbName).getCollection(collName2);
const recipientColl2 = recipientPrimary.getDB(dbName).getCollection(collName2);
tenantMigrationTest.insertDonorDB(dbName, collName2, [{_id: 0, a: {b: 10, c: 10}}]);
assert.commandWorked(donorColl2.createIndex({"a.b": 1, "a.c": 1}));
// Since it is not multi-key index yet, both multi-key paths should be empty.
verifyMultiKeyIndex(donorColl2, false, {"a.b": [], "a.c": []});

const migrationId = UUID();
const migrationIdString = extractUUIDFromObject(migrationId);
const migrationOpts = {
    migrationIdString: migrationIdString,
    recipientConnString: tenantMigrationTest.getRecipientConnString(),
    tenantId: tenantId,
};

// Configure a failpoint to hang after recipient data becomes consistent.
const fpBeforeFulfillingDataConsistentPromise = configureFailPoint(
    recipientPrimary, "fpBeforeFulfillingDataConsistentPromise", {action: "hang"});

jsTestLog("Starting the tenant migration");
assert.commandWorked(
    tenantMigrationTest.startMigration(migrationOpts, {enableDonorStartMigrationFsync: true}));

fpBeforeFulfillingDataConsistentPromise.wait();

// At this point, recipient data is consistent, so both collections on the recipient
// side should have the same multi-key state as they were in donor side.
verifyMultiKeyIndex(recipientColl1, true, {"a.b": ["a", "a.b"], "a.c": ["a"]});
verifyMultiKeyIndex(recipientColl2, false, {"a.b": [], "a.c": []});

// Update an entry in collection 1 on donor side, that make "a.c" to be an array as well.
// The recipient should continue fetching before migration finishes and thus change the
// multi-key path on "a.c" after it becomes consistent again.
assert.commandWorked(donorColl1.update(
    {_id: 1, "a.b": 50}, {$set: {"a.$.c": [60, 70]}}, {writeConcern: {w: 'majority'}}));
// Now the index on donor's collection 1 should still be multi-key, but the multi-key path
// on "a.c" should be changed, recipient should be the same after migration finishes.
verifyMultiKeyIndex(donorColl1, true, {"a.b": ["a", "a.b"], "a.c": ["a", "a.c"]});

// Insert new entries into collection 2 on donor side, that turns its index into multi-key.
// The recipient should continue fetching before migration finishes and likewise turn the
// index on its collection 2 into multi-key after it becomes consistent again.
tenantMigrationTest.insertDonorDB(
    dbName, collName2, [{_id: 1, a: [{b: [20, 30], c: 40}, {b: 50, c: 60}]}]);
// Now the index on donor's collection 2 should be multi-key, recipient should be the same
// after migration finishes.
verifyMultiKeyIndex(donorColl2, true, {"a.b": ["a", "a.b"], "a.c": ["a"]});

fpBeforeFulfillingDataConsistentPromise.off();

// Wait for tenant migration to finish.
TenantMigrationTest.assertCommitted(tenantMigrationTest.waitForMigrationToComplete(migrationOpts));

// Recipient now should have fetched the newly updated data, and changed the multi-key path
// on "a.c" in collection 1.
verifyMultiKeyIndex(recipientColl1, true, {"a.b": ["a", "a.b"], "a.c": ["a", "a.c"]});
// Recipient now should have fetched newly inserted data, and turned the index on its
// collection 2 into multi-key, with the same multi-key path as the donor's.
verifyMultiKeyIndex(recipientColl2, true, {"a.b": ["a", "a.b"], "a.c": ["a"]});

tenantMigrationTest.stop();
recipientRst.stopSet();
})();
