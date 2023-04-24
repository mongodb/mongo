/**
 * Tests that recipient is able to copy and apply change collection entries from the donor for the
 * shard merge protocol.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_fcv_71,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 *   featureFlagShardMerge
 * ]
 */

import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";
import {
    isShardMergeEnabled,
    makeX509OptionsForTest
} from "jstests/replsets/libs/tenant_migration_util.js";

// For assertDropAndRecreateCollection.
load("jstests/libs/collection_drop_recreate.js");
load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_util.js");
load("jstests/serverless/libs/change_collection_util.js");

function setup() {
    const donorRst = new ChangeStreamMultitenantReplicaSetTest({
        name: "donorReplSet",
        nodes: 2,
        nodeOptions: Object.assign(makeX509OptionsForTest().donor, {
            setParameter: {
                tenantMigrationGarbageCollectionDelayMS: 0,
                ttlMonitorSleepSecs: 1,
            }
        }),
    });
    const recipientRst = new ChangeStreamMultitenantReplicaSetTest({
        name: "recipientReplSet",
        nodes: 2,
        nodeOptions: Object.assign(makeX509OptionsForTest().recipient, {
            setParameter: {
                tenantMigrationGarbageCollectionDelayMS: 0,
                ttlMonitorSleepSecs: 1,
            }
        }),
    });

    const tenantMigrationTest = new TenantMigrationTest({
        name: jsTestName(),
        donorRst,
        recipientRst,
        quickGarbageCollection: true,
    });

    const teardown = () => {
        donorRst.stopSet();
        recipientRst.stopSet();
        tenantMigrationTest.stop();
    };

    // Note: including this explicit early return here due to the fact that multiversion
    // suites will execute this test without featureFlagShardMerge enabled (despite the
    // presence of the featureFlagShardMerge tag above), which means the test will attempt
    // to run a multi-tenant migration and fail.
    if (!isShardMergeEnabled(recipientRst.getPrimary().getDB("admin"))) {
        teardown();
        jsTestLog("Skipping Shard Merge-specific test");
        quit();
    }

    return {
        donorRst,
        recipientRst,
        tenantMigrationTest,
        teardown,
    };
}

function assertChangeCollectionEntries(donorEntries, recipientEntries) {
    assert.eq(donorEntries.length, recipientEntries.length);
    donorEntries.forEach((donorEntry, idx) => {
        assert.eq(donorEntry, recipientEntries[idx]);
    });
}

function validateChangeCollections(tenantId, donorTenantConn, recipientConns) {
    const donorChangeCollectionDocuments = getChangeCollectionDocuments(donorTenantConn);

    recipientConns.forEach(recipientConn => {
        jsTestLog(
            `Performing change collection validation for tenant ${tenantId} on ${recipientConn}`);
        assertChangeCollectionEntries(donorChangeCollectionDocuments,
                                      getChangeCollectionDocuments(recipientConn));
    });
}

function assertCursorChangeEvents(expectedEvents, cursors) {
    expectedEvents.forEach(expectedEvent => {
        cursors.forEach(cursor => {
            assert.soon(() => cursor.hasNext());
            const changeEvent = cursor.next();
            assert.eq(changeEvent.documentKey._id, expectedEvent._id);
            assert.eq(changeEvent.operationType, expectedEvent.operationType);
            if (expectedEvent.fullDocument) {
                assert.eq(changeEvent.fullDocument, expectedEvent.fullDocument);
            }
            if (expectedEvent.fullDocumentBeforeChange) {
                assert.eq(changeEvent.fullDocumentBeforeChange,
                          expectedEvent.fullDocumentBeforeChange);
            }
        });
    });
}

function assertChangeStreamGetMoreFailure(donorConnections) {
    // Test that running a getMore on a change stream cursor after the migration commits throws
    // a resumable change stream exception.
    donorConnections.forEach(({conn, cursor}) => {
        const failedGetMore = conn.getDB("database").runCommand("getMore", {
            getMore: cursor._cursorid,
            collection: "collection"
        });
        assert.commandFailedWithCode(
            failedGetMore,
            ErrorCodes.ResumeTenantChangeStream,
            "Tailing a change stream on the donor after completion of a shard merge should fail.");
        assert(failedGetMore.hasOwnProperty("errorLabels"));
        assert.contains("ResumableChangeStreamError", failedGetMore.errorLabels);

        // The cursor should have been deleted after the error so a getMore should fail.
        assert.commandFailedWithCode(
            conn.getDB("database")
                .runCommand("getMore", {getMore: cursor._cursorid, collection: "collection"}),
            ErrorCodes.CursorNotFound);
    });
}

function getTenantConnections(rst, tenantId) {
    const primaryConn = ChangeStreamMultitenantReplicaSetTest.getTenantConnection(
        rst.getPrimary().host, tenantId, tenantId.str);

    // Running ChangeStreamMultitenantReplicaSetTest.getTenantConnection will create a user on the
    // primary. Await replication so that we can use the same user on secondaries.
    rst.awaitReplication();

    const secondaryConns = rst.getSecondaries().map(
        recipientSecondary => ChangeStreamMultitenantReplicaSetTest.getTenantConnection(
            recipientSecondary.host, tenantId, tenantId.str));

    return [primaryConn, ...secondaryConns];
}

function performWrites(conn, _id) {
    const collection = conn.getDB("database").collection;
    assert.commandWorked(collection.insertOne({_id}));
    collection.updateOne({_id}, {$set: {updated: true}});
    collection.deleteOne({_id});
}

function performRetryableWrites(conn, _id) {
    const session = conn.startSession({retryWrites: true});
    const collection = session.getDatabase("database").collection;
    assert.commandWorked(collection.insert({_id, w: "RETRYABLE"}));
    assert.commandWorked(session.getDatabase("database").runCommand({
        findAndModify: "collection",
        query: {_id},
        update: {$set: {updated: true}}
    }));
    assert.commandWorked(collection.deleteOne({_id}));
}

function performTxnWrites(conn, _id) {
    const session = conn.getDB("database").getMongo().startSession();
    session.startTransaction();
    session.getDatabase("database").collection.insertOne({_id});
    session.getDatabase("database").collection.updateOne({_id}, {$set: {updated: true}});
    session.getDatabase("database").collection.deleteOne({_id});
    session.commitTransaction();
    session.endSession();
}

function performLargeTxnWrites(conn, _id) {
    const largePad = "a".repeat(10 * 1024 * 1024);
    const session = conn.getDB("database").getMongo().startSession();
    session.startTransaction();
    session.getDatabase("database").collection.insertOne({_id, largePad});
    session.getDatabase("database").collection.updateOne({_id}, {
        $set: {updated: true, largePad: `b${largePad}`}
    });
    session.commitTransaction();
}

function getChangeCollectionDocuments(conn) {
    // Filter out change collection entries for admin.system.users because 'getTenantConnection'
    // will create a user on the donor before we have enabled change streams. Also filter out
    // 'create' entries for system.change_collection, since the recipient will have an extra
    // entry for the case where changestreams are enabled for a tenant during oplog catchup.
    return conn.getDB("config")["system.change_collection"]
        .find({ns: {$ne: "admin.system.users"}})
        .toArray();
}

(() => {
    jsTestLog("Test writes before and during the migration with pre and post images enabled");
    const {tenantMigrationTest, donorRst, recipientRst, teardown} = setup();

    const donorPrimary = tenantMigrationTest.getDonorPrimary();
    const recipientPrimary = tenantMigrationTest.getRecipientPrimary();

    const tenantId1 = ObjectId();
    const tenantId2 = ObjectId();

    const donorTenantConn1 = ChangeStreamMultitenantReplicaSetTest.getTenantConnection(
        donorPrimary.host, tenantId1, tenantId1.str);

    donorRst.setChangeStreamState(donorTenantConn1, true);

    assertDropAndRecreateCollection(donorTenantConn1.getDB("database"),
                                    "collection",
                                    {changeStreamPreAndPostImages: {enabled: true}});

    // Open a change stream and perform writes before the migration starts.
    const donorCursor1 = donorTenantConn1.getDB("database").collection.watch([]);
    donorTenantConn1.getDB("database").collection.insertOne({_id: "tenant1_0"});
    performWrites(donorTenantConn1, "tenant1_1");
    performRetryableWrites(donorTenantConn1, "tenant1_2");
    performTxnWrites(donorTenantConn1, "tenant1_in_transaction_1");

    // Get the first entry from the tenant1 change stream cursor and grab the resume token.
    assert.soon(() => donorCursor1.hasNext());
    const {_id: resumeToken1} = donorCursor1.next();

    const fpBeforeMarkingCloneSuccess =
        configureFailPoint(recipientPrimary, "fpBeforeMarkingCloneSuccess", {action: "hang"});

    const migrationUuid = UUID();
    const tenantIds = [tenantId1, tenantId2];
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationUuid),
        readPreference: {mode: "primary"},
        tenantIds,
    };

    assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));

    fpBeforeMarkingCloneSuccess.wait();

    // Perform more writes after cloning has completed so that oplog entries are applied during
    // oplog catchup.
    performWrites(donorTenantConn1, "tenant1_3");
    performRetryableWrites(donorTenantConn1, "tenant1_4");

    // Enable change streams for the second tenant during oplog catchup.
    const donorTenantConn2 = ChangeStreamMultitenantReplicaSetTest.getTenantConnection(
        donorPrimary.host, tenantId2, tenantId2.str);

    donorRst.setChangeStreamState(donorTenantConn2, true);
    assertDropAndRecreateCollection(donorTenantConn2.getDB("database"),
                                    "collection",
                                    {changeStreamPreAndPostImages: {enabled: true}});

    const donorCursor2 = donorTenantConn2.getDB("database").collection.watch([]);
    donorTenantConn2.getDB("database").collection.insertOne({_id: "tenant2_0"});
    donorTenantConn2.getDB("database").collection.insertOne({_id: "tenant2_1"});

    // Get the first entry from the tenant2 change stream cursor and grab the resume token.
    assert.soon(() => donorCursor2.hasNext());
    const {_id: resumeToken2} = donorCursor2.next();

    performTxnWrites(donorTenantConn2, "tenant2_in_transaction_1");

    fpBeforeMarkingCloneSuccess.off();

    TenantMigrationTest.assertCommitted(
        tenantMigrationTest.waitForMigrationToComplete(migrationOpts));

    assertChangeStreamGetMoreFailure([
        {conn: donorTenantConn1, cursor: donorCursor1},
        {conn: donorTenantConn2, cursor: donorCursor2},
    ]);

    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
    tenantMigrationTest.waitForMigrationGarbageCollection(migrationUuid, tenantIds[0]);

    const recipientTenant1Conns = getTenantConnections(recipientRst, tenantId1);
    validateChangeCollections(tenantId1, donorTenantConn1, recipientTenant1Conns);

    const tenantConn1Cursors =
        recipientTenant1Conns.map(conn => conn.getDB("database").collection.watch([], {
            resumeAfter: resumeToken1,
            fullDocumentBeforeChange: "required",
            fullDocument: "required",
        }));

    assertCursorChangeEvents(
        [
            {_id: "tenant1_1", operationType: "insert", fullDocument: {_id: "tenant1_1"}},
            {
                _id: "tenant1_1",
                operationType: "update",
                fullDocumentBeforeChange: {_id: "tenant1_1"},
                fullDocument: {_id: "tenant1_1", updated: true}
            },
            {
                _id: "tenant1_1",
                operationType: "delete",
                fullDocumentBeforeChange: {_id: "tenant1_1", updated: true},
            },
            {
                _id: "tenant1_2",
                operationType: "insert",
                fullDocument: {_id: "tenant1_2", w: "RETRYABLE"}
            },
            {
                _id: "tenant1_2",
                operationType: "update",
                fullDocumentBeforeChange: {_id: "tenant1_2", "w": "RETRYABLE"},
                fullDocument: {_id: "tenant1_2", "w": "RETRYABLE", updated: true}
            },
            {
                _id: "tenant1_2",
                operationType: "delete",
                fullDocumentBeforeChange: {_id: "tenant1_2", "w": "RETRYABLE", updated: true}
            },
            {
                _id: "tenant1_in_transaction_1",
                operationType: "insert",
                fullDocument: {_id: "tenant1_in_transaction_1"}
            },
            {
                _id: "tenant1_in_transaction_1",
                operationType: "update",
                fullDocumentBeforeChange: {_id: "tenant1_in_transaction_1"},
                fullDocument: {_id: "tenant1_in_transaction_1", updated: true}
            },
            {
                _id: "tenant1_in_transaction_1",
                operationType: "delete",
                fullDocumentBeforeChange: {_id: "tenant1_in_transaction_1", updated: true}
            },
            {_id: "tenant1_3", operationType: "insert", fullDocument: {_id: "tenant1_3"}},
            {
                _id: "tenant1_3",
                operationType: "update",
                fullDocumentBeforeChange: {_id: "tenant1_3"},
                fullDocument: {_id: "tenant1_3", updated: true}
            },
            {
                _id: "tenant1_3",
                operationType: "delete",
                fullDocumentBeforeChange: {_id: "tenant1_3", updated: true},
            },
            {
                _id: "tenant1_4",
                operationType: "insert",
                fullDocument: {_id: "tenant1_4", w: "RETRYABLE"}
            },
            {
                _id: "tenant1_4",
                operationType: "update",
                fullDocumentBeforeChange: {_id: "tenant1_4", w: "RETRYABLE"},
                fullDocument: {_id: "tenant1_4", w: "RETRYABLE", updated: true}
            },
            {
                _id: "tenant1_4",
                operationType: "delete",
                fullDocumentBeforeChange: {_id: "tenant1_4", w: "RETRYABLE", updated: true},
            },
        ],
        tenantConn1Cursors);

    const recipientTenant2Conns = getTenantConnections(recipientRst, tenantId2);
    validateChangeCollections(tenantId2, donorTenantConn2, recipientTenant2Conns);

    const tenantConn2Cursors =
        recipientTenant2Conns.map(conn => conn.getDB("database").collection.watch([], {
            resumeAfter: resumeToken2,
            fullDocumentBeforeChange: "required",
            fullDocument: "required",
        }));

    assertCursorChangeEvents(
        [
            {_id: "tenant2_1", operationType: "insert", fullDocument: {_id: "tenant2_1"}},
            {
                _id: "tenant2_in_transaction_1",
                operationType: "insert",
                fullDocument: {_id: "tenant2_in_transaction_1"}
            },
            {
                _id: "tenant2_in_transaction_1",
                operationType: "update",
                fullDocumentBeforeChange: {_id: "tenant2_in_transaction_1"},
                fullDocument: {_id: "tenant2_in_transaction_1", updated: true}
            },
            {
                _id: "tenant2_in_transaction_1",
                operationType: "delete",
                fullDocumentBeforeChange: {_id: "tenant2_in_transaction_1", updated: true},
            },
        ],
        tenantConn2Cursors);

    teardown();
})();

(() => {
    jsTestLog("Test large txns before and during the migration");
    const {tenantMigrationTest, donorRst, recipientRst, teardown} = setup();

    const donorPrimary = tenantMigrationTest.getDonorPrimary();
    const recipientPrimary = tenantMigrationTest.getRecipientPrimary();

    const tenantId1 = ObjectId();
    const tenantId2 = ObjectId();

    const donorTenantConn1 = ChangeStreamMultitenantReplicaSetTest.getTenantConnection(
        donorPrimary.host, tenantId1, tenantId1.str);

    donorRst.setChangeStreamState(donorTenantConn1, true);

    // Open a change stream and perform writes before the migration starts.
    const donorCursor1 = donorTenantConn1.getDB("database").collection.watch([]);
    donorTenantConn1.getDB("database").collection.insertOne({_id: "tenant1_0"});
    performLargeTxnWrites(donorTenantConn1, "tenant1_in_transaction_1");

    // Get the first entry from the tenant1 change stream cursor and grab the resume token.
    assert.soon(() => donorCursor1.hasNext());
    const {_id: resumeToken1} = donorCursor1.next();

    const fpBeforeMarkingCloneSuccess =
        configureFailPoint(recipientPrimary, "fpBeforeMarkingCloneSuccess", {action: "hang"});

    const migrationUuid = UUID();
    const tenantIds = [tenantId1, tenantId2];
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationUuid),
        readPreference: {mode: "primary"},
        tenantIds,
    };

    assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));

    fpBeforeMarkingCloneSuccess.wait();

    // Perform more writes after cloning has completed so that oplog entries are applied during
    // oplog catchup.
    performLargeTxnWrites(donorTenantConn1, "tenant1_in_transaction_2");

    // Enable change streams for the second tenant during oplog catchup.
    const donorTenantConn2 = ChangeStreamMultitenantReplicaSetTest.getTenantConnection(
        donorPrimary.host, tenantId2, tenantId2.str);

    donorRst.setChangeStreamState(donorTenantConn2, true);

    const donorCursor2 = donorTenantConn2.getDB("database").collection.watch([]);
    donorTenantConn2.getDB("database").collection.insertOne({_id: "tenant2_0"});
    performLargeTxnWrites(donorTenantConn2, "tenant2_in_transaction_1");

    // Get the first entry from the tenant2 change stream cursor and grab the resume token.
    assert.soon(() => donorCursor2.hasNext());
    const {_id: resumeToken2} = donorCursor2.next();

    fpBeforeMarkingCloneSuccess.off();

    TenantMigrationTest.assertCommitted(
        tenantMigrationTest.waitForMigrationToComplete(migrationOpts));

    assertChangeStreamGetMoreFailure([
        {conn: donorTenantConn1, cursor: donorCursor1},
        {conn: donorTenantConn2, cursor: donorCursor2},
    ]);

    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
    tenantMigrationTest.waitForMigrationGarbageCollection(migrationUuid, tenantIds[0]);

    const recipientTenant1Conns = getTenantConnections(recipientRst, tenantId1);
    validateChangeCollections(tenantId1, donorTenantConn1, recipientTenant1Conns);

    const tenantConn1Cursors = recipientTenant1Conns.map(
        conn => conn.getDB("database").collection.watch([{$unset: "largePad"}], {
            resumeAfter: resumeToken1,
        }));

    assertCursorChangeEvents(
        [
            {
                _id: "tenant1_in_transaction_1",
                operationType: "insert",
            },
            {
                _id: "tenant1_in_transaction_1",
                operationType: "update",
            },
            {
                _id: "tenant1_in_transaction_2",
                operationType: "insert",
            },
            {
                _id: "tenant1_in_transaction_2",
                operationType: "update",
            },
        ],
        tenantConn1Cursors);

    const recipientTenant2Conns = getTenantConnections(recipientRst, tenantId2);
    validateChangeCollections(tenantId2, donorTenantConn2, recipientTenant2Conns);

    const tenantConn2Cursors = recipientTenant2Conns.map(
        conn => conn.getDB("database").collection.watch([{$unset: "largePad"}], {
            resumeAfter: resumeToken2,
        }));

    assertCursorChangeEvents(
        [
            {
                _id: "tenant2_in_transaction_1",
                operationType: "insert",
            },
            {
                _id: "tenant2_in_transaction_1",
                operationType: "update",
            },
        ],
        tenantConn2Cursors);

    teardown();
})();
