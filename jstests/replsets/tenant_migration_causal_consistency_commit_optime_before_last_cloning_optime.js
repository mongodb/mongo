/**
 * Verify that causal consistency is respected if a tenant migration commits with an earlier optime
 * timestamp than the latest optime associated with cloning on the recipient.
 *
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_shard_merge,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 * ]
 */

import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";
import {makeX509OptionsForTest} from "jstests/replsets/libs/tenant_migration_util.js";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/rslib.js");

function assertCanFindWithReadConcern(conn, dbName, collName, expectedDoc, readConcern) {
    const res = assert.commandWorked(conn.getDB(dbName).runCommand({find: collName, readConcern}));
    assert(res.cursor.firstBatch, tojson(res));
    assert.eq(1, res.cursor.firstBatch.length, tojson(res));
    assert.eq(expectedDoc, res.cursor.firstBatch[0], tojson(res));
}

// Local read concern case.
(() => {
    const migrationX509Options = makeX509OptionsForTest();

    // Simulate a lagged node by setting secondaryDelaySecs on one recipient secondary. Verify this
    // does not prevent reading all the tenant's data after the migration commits.
    const recipientRst = new ReplSetTest({
        name: "recipient_local_case",
        nodes: 3,
        serverless: true,
        nodeOptions: migrationX509Options.recipient
    });
    recipientRst.startSet();

    let config = recipientRst.getReplSetConfig();
    config.members[2].priority = 0;
    config.members[2].secondaryDelaySecs = 5;
    recipientRst.initiate(config);

    const normalSecondary = recipientRst.getSecondaries()[0];
    const laggedSecondary = recipientRst.getSecondaries()[1];

    const tmt = new TenantMigrationTest({name: jsTestName(), recipientRst});

    const tenantId = ObjectId().str;
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(UUID()),
        tenantId,
    };

    const dbName = `${tenantId}_test`;
    const collName = "foo";

    // Insert tenant data to be copied. Save the operationTime to use for afterClusterTime reads
    // later, simulating a client in a causally consistent session.
    const insertRes = assert.commandWorked(tmt.getDonorPrimary().getDB(dbName).runCommand(
        "insert", {insert: collName, documents: [{_id: 0, x: 0}]}));
    assert(insertRes.operationTime, tojson(insertRes));

    let hangFp = configureFailPoint(tmt.getRecipientPrimary(),
                                    "fpAfterPersistingTenantMigrationRecipientInstanceStateDoc",
                                    {action: "hang"});
    assert.commandWorked(tmt.startMigration(migrationOpts));
    hangFp.wait();

    // Do writes on the recipient to advance its cluster time past the donor's.
    let bulk = tmt.getRecipientPrimary().getDB("unrelatedDB").bar.initializeUnorderedBulkOp();
    for (let i = 0; i < 100; i++) {
        bulk.insert({x: i});
    }
    assert.commandWorked(bulk.execute());

    // Allow the migration to complete. The cloned op should be written with a later opTime on the
    // recipient than the migration commits with on the donor.
    hangFp.off();
    TenantMigrationTest.assertCommitted(tmt.waitForMigrationToComplete(migrationOpts));

    // Local reads should always see all the tenant's data, with or without afterClusterTime.
    assertCanFindWithReadConcern(
        laggedSecondary, dbName, collName, {_id: 0, x: 0}, {level: "local"});
    assertCanFindWithReadConcern(laggedSecondary,
                                 dbName,
                                 collName,
                                 {_id: 0, x: 0},
                                 {level: "local", afterClusterTime: insertRes.operationTime});

    assertCanFindWithReadConcern(
        normalSecondary, dbName, collName, {_id: 0, x: 0}, {level: "local"});
    assertCanFindWithReadConcern(normalSecondary,
                                 dbName,
                                 collName,
                                 {_id: 0, x: 0},
                                 {level: "local", afterClusterTime: insertRes.operationTime});

    recipientRst.stopSet();
    tmt.stop();
})();

// Majority read concern case.
(() => {
    const tmt = new TenantMigrationTest({name: jsTestName(), sharedOptions: {nodes: 3}});

    const tenantId = ObjectId().str;
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(UUID()),
        tenantId,
    };

    const dbName = `${tenantId}_test`;
    const collName = "foo";

    // Insert tenant data to be copied.
    const insertRes = assert.commandWorked(tmt.getDonorPrimary().getDB(dbName).runCommand(
        "insert", {insert: collName, documents: [{_id: 0, x: 0}]}));
    assert(insertRes.operationTime, tojson(insertRes));

    // Simulate a lagged node by disabling snapshotting on a secondary so it cannot advance its
    // majority committed snapshot to include the block timestamp. Verify this prevents reading
    // from it until the majority snapshot advances.
    const laggedSecondary = tmt.getRecipientRst().getSecondaries()[0];
    const normalSecondary = tmt.getRecipientRst().getSecondaries()[1];

    let hangFp = configureFailPoint(tmt.getRecipientPrimary(),
                                    "fpAfterPersistingTenantMigrationRecipientInstanceStateDoc",
                                    {action: "hang"});

    assert.commandWorked(tmt.startMigration(migrationOpts));
    hangFp.wait();

    tmt.getRecipientRst().awaitLastOpCommitted();
    const snapshotFp = configureFailPoint(laggedSecondary, "disableSnapshotting");

    // Do writes on the recipient to advance its cluster time past the donor's and advance the
    // majority committed snapshot on the non-lagged nodes.
    let bulk = tmt.getRecipientPrimary().getDB("unrelatedDB").bar.initializeUnorderedBulkOp();
    for (let i = 0; i < 100; i++) {
        bulk.insert({x: i});
    }
    assert.commandWorked(bulk.execute());

    // Allow the migration to complete. The cloned op should commit with a later opTime on the
    // recipient than the migration commits with on the donor.
    hangFp.off();
    TenantMigrationTest.assertCommitted(tmt.waitForMigrationToComplete(migrationOpts));

    // Verify majority reads cannot be served on the lagged recipient secondary with or without
    // afterClusterTime because its majority committed snapshot is behind the block timestamp.
    assert.commandFailedWithCode(
        laggedSecondary.getDB(dbName).runCommand(
            {find: collName, readConcern: {level: "majority"}, maxTimeMS: 2000}),
        ErrorCodes.MaxTimeMSExpired);
    assert.commandFailedWithCode(laggedSecondary.getDB(dbName).runCommand({
        find: collName,
        readConcern: {level: "majority", afterClusterTime: insertRes.operationTime},
        maxTimeMS: 2000
    }),
                                 ErrorCodes.MaxTimeMSExpired);

    // Reads on the normal secondary should succeed.
    assertCanFindWithReadConcern(
        normalSecondary, dbName, collName, {_id: 0, x: 0}, {level: "majority"});
    assertCanFindWithReadConcern(normalSecondary,
                                 dbName,
                                 collName,
                                 {_id: 0, x: 0},
                                 {level: "majority", afterClusterTime: insertRes.operationTime});

    // When the snapshot is allowed to advance, verify majority reads can now be served on all
    // nodes. Insert to trigger advancing the committed snapshot.
    snapshotFp.off();
    assert.commandWorked(tmt.getRecipientPrimary().getDB(collName).bar.insert({x: "unrelated"}));

    assertCanFindWithReadConcern(
        laggedSecondary, dbName, collName, {_id: 0, x: 0}, {level: "majority"});
    assertCanFindWithReadConcern(laggedSecondary,
                                 dbName,
                                 collName,
                                 {_id: 0, x: 0},
                                 {level: "majority", afterClusterTime: insertRes.operationTime});

    assertCanFindWithReadConcern(
        normalSecondary, dbName, collName, {_id: 0, x: 0}, {level: "majority"});
    assertCanFindWithReadConcern(normalSecondary,
                                 dbName,
                                 collName,
                                 {_id: 0, x: 0},
                                 {level: "majority", afterClusterTime: insertRes.operationTime});
    tmt.stop();
})();

// Snapshot read concern is tested in replsets/tenant_migration_concurrent_reads_on_recipient.js
