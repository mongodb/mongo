/**
 * Verifies that nodes can trigger noop writes to satisfy cluster time reads after a tenant
 * migration.
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

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");
load("jstests/libs/write_concern_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");
load('jstests/libs/parallel_shell_helpers.js');

const kTenantIdPrefix = "testTenantId";
// During "shard merge" tenant migrations, writes to internal DBs are still allowed.
const kUnrelatedDbName = "admin";
const collName = "foo";
const migrationX509Options = TenantMigrationUtil.makeX509OptionsForTest();

let counter = 0;
let makeTenantId = function() {
    return kTenantIdPrefix + "-" + counter++;
};

function makeTestParams() {
    const tenantId = makeTenantId();
    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: tenantId,
    };
    const dbName = tenantId + "_db";
    return [tenantId, migrationId, migrationOpts, dbName];
}

function advanceClusterTime(conn, dbName, collName) {
    let bulk = conn.getDB(dbName)[collName].initializeUnorderedBulkOp();
    for (let i = 0; i < 200; i++) {
        bulk.insert({x: i});
    }
    assert.commandWorked(bulk.execute());
}

function getBlockTimestamp(conn, tenantId) {
    const mtabServerStatus =
        TenantMigrationUtil.getTenantMigrationAccessBlocker({donorNode: conn, tenantId}).donor;
    assert(mtabServerStatus.blockTimestamp, tojson(mtabServerStatus));
    return mtabServerStatus.blockTimestamp;
}

function runAfterClusterTimeRead(dbName, collName, operationTime, clusterTime, expectedCode) {
    db.getMongo().setSecondaryOk();
    const res = db.getSiblingDB(dbName).runCommand({
        find: collName,
        readConcern: {afterClusterTime: operationTime},
        $clusterTime: clusterTime
    });
    if (expectedCode) {
        assert.commandFailedWithCode(res, expectedCode);
    } else {
        assert.commandWorked(res);
    }
}

function setup() {
    const donorRst = new ReplSetTest({
        nodes: 3,
        name: "donor",
        settings: {chainingAllowed: false},
        nodeOptions: Object.assign(migrationX509Options.donor, {
            setParameter: {
                // To allow after test hooks to run without errors.
                "failpoint.tenantMigrationDonorAllowsNonTimestampedReads":
                    tojson({mode: "alwaysOn"}),
            }
        })
    });
    donorRst.startSet();
    donorRst.initiate();

    const recipientRst = new ReplSetTest({
        nodes: 3,
        name: "recipient",
        settings: {chainingAllowed: false},
        nodeOptions: migrationX509Options.recipient
    });
    recipientRst.startSet();
    recipientRst.initiate();

    const tmt = new TenantMigrationTest({name: jsTestName(), donorRst, recipientRst});

    return {
        tmt,
        teardown: function() {
            donorRst.stopSet();
            recipientRst.stopSet();
            tmt.stop();
        },
    };
}

{
    jsTestLog("Testing noops on the recipient");

    const {tmt, teardown} = setup();

    const [tenantId, migrationId, migrationOpts, tenantDbName] = makeTestParams();
    const laggedRecipientSecondary = tmt.getRecipientRst().getSecondary();
    const fp =
        configureFailPoint(tmt.getDonorPrimary(), "pauseTenantMigrationBeforeLeavingBlockingState");

    //
    // Run a migration, pausing after selecting a block timestamp to advance cluster time beyond it
    // on the donor.
    //

    tmt.insertDonorDB(tenantDbName, collName);
    assert.commandWorked(tmt.startMigration(migrationOpts));

    fp.wait();

    // Disable replication on a recipient secondary so it cannot advance its last applied opTime
    // beyond the latest time on the donor from unrelated writes. The block timestamp will have
    // already been replicated by this point.
    stopServerReplication(laggedRecipientSecondary);

    advanceClusterTime(tmt.getDonorPrimary(), kUnrelatedDbName, collName);

    const donorRes = assert.commandWorked(
        tmt.getDonorPrimary().getDB(tenantDbName).runCommand({find: collName}));
    assert(donorRes.operationTime, tojson(donorRes));
    assert.eq(
        timestampCmp(donorRes.operationTime, getBlockTimestamp(tmt.getDonorPrimary(), tenantId)),
        1,
        tojson(donorRes));

    fp.off();
    TenantMigrationTest.assertCommitted(tmt.waitForMigrationToComplete(
        migrationOpts, false /* retryOnRetryableErrors */, true /* forgetMigration */));

    //
    // Verify reading on the recipient with an afterClusterTime > the block timestamp
    // triggers a noop write on the recipient primary. Unrelated writes on the primary may
    // prevent the noop from taking effect, so we can't check the oplog. appendOplogNote isn't
    // profiled so we use a fail point to detect it.
    //

    const hangInNoopFp = configureFailPoint(tmt.getRecipientPrimary(), "hangInAppendOplogNote");
    const awaitReadOnRecipient = startParallelShell(funWithArgs(runAfterClusterTimeRead,
                                                                tenantDbName,
                                                                collName,
                                                                donorRes.operationTime,
                                                                donorRes.$clusterTime),
                                                    laggedRecipientSecondary.port);

    hangInNoopFp.wait();
    hangInNoopFp.off();

    restartServerReplication(laggedRecipientSecondary);
    awaitReadOnRecipient();
    teardown();
}

{
    jsTestLog("Testing noops on the donor");

    const {tmt, teardown} = setup();

    const [tenantId, migrationId, migrationOpts, tenantDbName] = makeTestParams();
    const laggedDonorSecondary = tmt.getDonorRst().getSecondary();
    const fp =
        configureFailPoint(tmt.getDonorPrimary(), "pauseTenantMigrationBeforeLeavingBlockingState");

    //
    // Commit a normal migration, but disable replication on a donor secondary before the commit so
    // it will not learn the outcome.
    //

    tmt.insertDonorDB(tenantDbName, collName);
    assert.commandWorked(tmt.startMigration(migrationOpts));

    fp.wait();

    stopServerReplication(laggedDonorSecondary);

    fp.off();
    TenantMigrationTest.assertCommitted(tmt.waitForMigrationToComplete(
        migrationOpts, false /* retryOnRetryableErrors */, true /* forgetMigration */));

    //
    // Advance cluster time on the recipient beyond the block timestamp.
    //

    advanceClusterTime(tmt.getRecipientPrimary(), kUnrelatedDbName, collName);

    const recipientRes = assert.commandWorked(
        tmt.getRecipientPrimary().getDB(tenantDbName).runCommand({find: collName}));
    assert(recipientRes.operationTime, tojson(recipientRes));
    assert.eq(timestampCmp(recipientRes.operationTime,
                           getBlockTimestamp(tmt.getDonorPrimary(), tenantId)),
              1,
              tojson(recipientRes));

    //
    // Verify reading from a lagged donor secondary with an afterClusterTime > the block timestamp
    // triggers a noop write on the donor primary. Even though reads later than the block timestamp
    // are rejected and the donor is guaranteed to eventually replicate the migration decision,
    // waiting for read concern is not interrupted upon learning the decision, so the noop is
    // necessary to unblock tenant operations waiting for a cluster time > the block timestamp.
    //

    const hangInNoopFp = configureFailPoint(tmt.getDonorPrimary(), "hangInAppendOplogNote");
    const awaitReadOnDonor = startParallelShell(funWithArgs(runAfterClusterTimeRead,
                                                            tenantDbName,
                                                            collName,
                                                            recipientRes.operationTime,
                                                            recipientRes.$clusterTime,
                                                            ErrorCodes.TenantMigrationCommitted),
                                                laggedDonorSecondary.port);

    hangInNoopFp.wait();
    hangInNoopFp.off();

    restartServerReplication(laggedDonorSecondary);
    awaitReadOnDonor();
    teardown();
}
})();
