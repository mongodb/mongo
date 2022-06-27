/**
 * Tests that in tenant migration, the recipient set can resume collection cloning from the last
 * document cloned after a failover.
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_shard_merge,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 * ]
 */

(function() {
"use strict";

const tenantMigrationFailoverTest = function(isTimeSeries, createCollFn, docs) {
    load("jstests/libs/fail_point_util.js");
    load("jstests/libs/uuid_util.js");  // for 'extractUUIDFromObject'
    load("jstests/replsets/libs/tenant_migration_test.js");
    load("jstests/replsets/libs/tenant_migration_util.js");

    const batchSize = 2;
    const recipientRst = new ReplSetTest({
        nodes: 2,
        name: jsTestName() + "_recipient",
        nodeOptions: Object.assign(TenantMigrationUtil.makeX509OptionsForTest().recipient, {
            setParameter: {
                // Use a batch size of 2 so that collection cloner requires more than a single
                // batch to complete.
                collectionClonerBatchSize: batchSize,
                // Allow reads on recipient before migration completes for testing.
                'failpoint.tenantMigrationRecipientNotRejectReads': tojson({mode: 'alwaysOn'}),
            }
        })
    });

    recipientRst.startSet();
    recipientRst.initiate();

    const tenantMigrationTest =
        new TenantMigrationTest({name: jsTestName(), recipientRst: recipientRst});
    const donorPrimary = tenantMigrationTest.getDonorPrimary();

    const tenantId = "testTenantId";
    const dbName = tenantMigrationTest.tenantDB(tenantId, "testDB");
    const donorDB = donorPrimary.getDB(dbName);
    const collName = "testColl";

    const recipientPrimary = tenantMigrationTest.getRecipientPrimary();

    // Create collection and insert documents.
    assert.commandWorked(createCollFn(donorDB, collName));
    tenantMigrationTest.insertDonorDB(dbName, collName, docs);

    const migrationId = UUID();
    const migrationIdString = extractUUIDFromObject(migrationId);
    const migrationOpts = {
        migrationIdString: migrationIdString,
        recipientConnString: tenantMigrationTest.getRecipientConnString(),
        tenantId: tenantId,
    };

    // Configure a fail point to have the recipient primary hang after cloning 2 documents.
    const recipientDb = recipientPrimary.getDB(dbName);
    let recipientColl = isTimeSeries ? recipientDb.getCollection("system.buckets." + collName)
                                     : recipientDb.getCollection(collName);

    const hangDuringCollectionClone =
        configureFailPoint(recipientDb,
                           "tenantMigrationHangCollectionClonerAfterHandlingBatchResponse",
                           {nss: recipientColl.getFullName()});

    // Start a migration and wait for recipient to hang after cloning 2 documents.
    assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));
    hangDuringCollectionClone.wait();
    assert.soon(() => recipientColl.find().itcount() === batchSize);

    // Insert some documents that will be fetched by the recipient. This is to test that on
    // failover, the fetcher will resume fetching from where it left off. The system is expected
    // to crash if the recipient fetches a duplicate oplog entry upon resuming the migration.
    tenantMigrationTest.insertDonorDB(dbName, "aNewColl", [{_id: "docToBeFetched"}]);
    assert.soon(() => {
        const configDb = recipientPrimary.getDB("config");
        const oplogBuffer = configDb.getCollection("repl.migration.oplog_" + migrationIdString);
        return oplogBuffer.find({"entry.o._id": "docToBeFetched"}).count() === 1;
    });

    // Step up a new node in the recipient set and trigger a failover. The new primary should resume
    // cloning starting from the third document.
    const newRecipientPrimary = recipientRst.getSecondaries()[0];
    recipientRst.stepUp(newRecipientPrimary);
    hangDuringCollectionClone.off();
    recipientRst.getPrimary();

    // The migration should go through after recipient failover.
    TenantMigrationTest.assertCommitted(
        tenantMigrationTest.waitForMigrationToComplete(migrationOpts));

    // Check that recipient has cloned all documents in the collection.
    recipientColl = newRecipientPrimary.getDB(dbName).getCollection(collName);
    assert.eq(docs.length, recipientColl.find().itcount());
    assert.docEq(recipientColl.find().sort({_id: 1}).toArray(), docs);
    TenantMigrationUtil.checkTenantDBHashes(
        tenantMigrationTest.getDonorRst(), tenantMigrationTest.getRecipientRst(), tenantId);

    tenantMigrationTest.stop();
    recipientRst.stopSet();
};

jsTestLog("Running tenant migration test for time-series collection");
tenantMigrationFailoverTest(true,
                            (db, collName) => db.createCollection(
                                collName, {timeseries: {timeField: "time", metaField: "bucket"}}),
                            [
                                // Group each document in its own bucket in order to work with the
                                // collectionClonerBatchSize we set at the recipient replSet.
                                {_id: 1, time: ISODate(), bucket: "a"},
                                {_id: 2, time: ISODate(), bucket: "b"},
                                {_id: 3, time: ISODate(), bucket: "c"},
                                {_id: 4, time: ISODate(), bucket: "d"}
                            ]);

jsTestLog("Running tenant migration test for regular collection");
tenantMigrationFailoverTest(false,
                            (db, collName) => db.createCollection(collName),
                            [{_id: 0}, {_id: "string"}, {_id: UUID()}, {_id: new Date()}]);
})();
