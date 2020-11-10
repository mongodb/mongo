/**
 * Tests that the client can retry commitTransaction on the tenant migration recipient.
 *
 * @tags: [requires_fcv_47, requires_majority_read_concern, incompatible_with_eft]
 */

(function() {
"use strict";

// Direct writes to config.transactions cannot be part of a session.
TestData.disableImplicitSessions = true;

load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");
load("jstests/replsets/rslib.js");
load("jstests/libs/uuid_util.js");

const donorRst = new ReplSetTest({
    nodes: 1,
    name: "donor",
    nodeOptions: {
        setParameter: {
            enableTenantMigrations: true,

            // Set the delay before a donor state doc is garbage collected to be short to speed up
            // the test.
            tenantMigrationGarbageCollectionDelayMS: 3 * 1000,

            // Set the TTL monitor to run at a smaller interval to speed up the test.
            ttlMonitorSleepSecs: 1,
        }
    }
});
const recipientRst = new ReplSetTest({
    nodes: [{}, {rsConfig: {priority: 0}}, {rsConfig: {priority: 0}}],
    name: "recipient",
    nodeOptions: {
        setParameter: {
            enableTenantMigrations: true,
            // TODO SERVER-51734: Remove the failpoint 'returnResponseOkForRecipientSyncDataCmd'.
            'failpoint.returnResponseOkForRecipientSyncDataCmd': tojson({mode: 'alwaysOn'})
        }
    }
});

donorRst.startSet();
donorRst.initiate();

recipientRst.startSet();
recipientRst.initiate();

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName(), donorRst, recipientRst});

const kTenantId = "testTenantId";
const kDbName = tenantMigrationTest.tenantDB(kTenantId, "testDB");
const kCollName = "testColl";
const kNs = `${kDbName}.${kCollName}`;

const donorPrimary = donorRst.getPrimary();
const recipientPrimary = recipientRst.getPrimary();

assert.commandWorked(donorPrimary.getCollection(kNs).insert(
    [{_id: 0, x: 0}, {_id: 1, x: 1}, {_id: 2, x: 2}], {writeConcern: {w: "majority"}}));

{
    jsTest.log("Run a transaction prior to the migration");
    const session = donorPrimary.startSession({causalConsistency: false});
    const sessionDb = session.getDatabase(kDbName);
    const sessionColl = sessionDb[kCollName];

    session.startTransaction({writeConcern: {w: "majority"}});
    const findAndModifyRes0 = sessionColl.findAndModify({query: {x: 0}, remove: true});
    assert.eq({_id: 0, x: 0}, findAndModifyRes0);
    assert.commandWorked(session.commitTransaction_forTesting());
    assert.sameMembers(sessionColl.find({}).toArray(), [{_id: 1, x: 1}, {_id: 2, x: 2}]);
    session.endSession();
}

let txnEntryOnDonor = donorPrimary.getCollection("config.transactions").find().toArray()[0];

jsTest.log("Run a migration to completion");
const migrationId = UUID();
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(migrationId),
    tenantId: kTenantId,
};
assert.commandWorked(tenantMigrationTest.runMigration(migrationOpts));

const donorDoc =
    donorPrimary.getCollection(TenantMigrationTest.kConfigDonorsNS).findOne({tenantId: kTenantId});

assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
tenantMigrationTest.waitForMigrationGarbageCollection(donorRst.nodes, migrationId, kTenantId);

{
    jsTest.log("Run another transaction after the migration");
    const session = donorPrimary.startSession({causalConsistency: false});
    const sessionDb = session.getDatabase(kDbName);
    const sessionColl = sessionDb[kCollName];

    session.startTransaction({writeConcern: {w: "majority"}});
    const findAndModifyRes1 = sessionColl.findAndModify({query: {x: 1}, remove: true});
    assert.eq({_id: 1, x: 1}, findAndModifyRes1);
    assert.commandWorked(session.commitTransaction_forTesting());
    assert.sameMembers(sessionColl.find({}).toArray(), [{_id: 2, x: 2}]);
    session.endSession();
}

// Test the aggregation pipeline the recipient would use for getting the config.transactions entry
// on the donor. The recipient will use the real startFetchingTimestamp, but this test uses the
// donor's commit timestamp as a substitute.
const startFetchingTimestamp = donorDoc.commitOrAbortOpTime.ts;
const aggRes = donorPrimary.getDB("config").runCommand({
    aggregate: "transactions",
    pipeline: [
        {$match: {"lastWriteOpTime.ts": {$lt: startFetchingTimestamp}, "state": "committed"}},
    ],
    readConcern: {level: "majority", afterClusterTime: startFetchingTimestamp},
    hint: "_id_",
    cursor: {},
});
assert.eq(1, aggRes.cursor.firstBatch.length);
assert.eq(txnEntryOnDonor, aggRes.cursor.firstBatch[0]);

// Test the client can retry commitTransaction for that transaction that committed prior to the
// migration.

// Insert the config.transactions entry on the recipient, but with a dummy lastWriteOpTime. The
// recipient should not need a real lastWriteOpTime to support a commitTransaction retry.
txnEntryOnDonor.lastWriteOpTime.ts = new Timestamp(0, 0);
assert.commandWorked(
    recipientPrimary.getCollection("config.transactions").insert([txnEntryOnDonor]));
recipientRst.awaitLastOpCommitted();
recipientRst.getSecondaries().forEach(node => {
    assert.eq(1, node.getCollection("config.transactions").count(txnEntryOnDonor));
});

assert.commandWorked(recipientPrimary.adminCommand({
    commitTransaction: 1,
    lsid: txnEntryOnDonor._id,
    txnNumber: txnEntryOnDonor.txnNum,
    autocommit: false
}));

donorRst.stopSet();
recipientRst.stopSet();
})();
