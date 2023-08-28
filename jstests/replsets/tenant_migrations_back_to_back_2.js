/**
 * This test simulates and verifies the handling of below edge case involving back-to-back tenant
 * migration (rs0 -> rs1 -> rs0) by both shard merge and tenant migration protocols.
 * 1) rs0: Retryable insert at txnNum: 55 succeeds.
 * 2) rs0: No-op session write (E.g. no-op retryable update) at txnNum: 56 succeeds, causing no
 *    writes to 'config.transactions' table but updates in-memory transaction participant.
 * 3) Start migration from rs0 -> rs1, copying the oplog chain for txnNum:55 from rs0 to rs1.
 * 4) rs0 -> rs1 migration succeeds.
 * 5) Starting a migration again from rs1 -> rs0 should succeed and not fail
 *    with ErrorCodes.TransactionTooOld.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 *   requires_fcv_71,
 * ]
 */

import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";
import {makeTenantDB} from "jstests/replsets/libs/tenant_migration_util.js";

const kMigrationId = UUID();
const kTenantId = ObjectId().str;
const kDbName = makeTenantDB(kTenantId, "testDb");
const kCollName = "testColl";
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(kMigrationId),
    tenantId: kTenantId,
};

const kSessionId = {
    id: UUID()
};
const kRetryableWriteTxnId = NumberLong(55);
const kNoopSessionWriteTxnId = NumberLong(kRetryableWriteTxnId + 1);

function runRetryableWriteWithTxnIdLessThanNoopWriteTxnId(conn) {
    return conn.getDB(kDbName).runCommand({
        insert: kCollName,
        documents: [{_id: "retryableWrite"}],
        txnNumber: kRetryableWriteTxnId,
        lsid: kSessionId,
        stmtIds: [NumberInt(0)]
    });
}

let noOpSessionWrites = [
    {
        testDesc: "no-op retryable write",
        testOp: (conn) => {
            assert.commandWorked(conn.getDB(kDbName).runCommand({
                update: kCollName,
                updates: [{q: {_id: "noOpRetryableWrite"}, u: {$inc: {x: 1}}}],
                txnNumber: kNoopSessionWriteTxnId,
                lsid: kSessionId,
                stmtIds: [NumberInt(1)]
            }));
        }
    },
    {
        testDesc: "read transaction",
        testOp: (conn) => {
            assert.commandWorked(conn.getDB(kDbName).runCommand({
                find: kCollName,
                txnNumber: kNoopSessionWriteTxnId,
                lsid: kSessionId,
                startTransaction: true,
                autocommit: false,
            }));
            assert.commandWorked(conn.getDB("admin").runCommand({
                commitTransaction: 1,
                txnNumber: kNoopSessionWriteTxnId,
                lsid: kSessionId,
                autocommit: false,
            }));
        }
    },
    {
        testDesc: "abort transaction",
        testOp: (conn) => {
            assert.commandWorked(conn.getDB(kDbName).runCommand({
                insert: kCollName,
                documents: [{_id: "noOpRetryableWrite"}],
                txnNumber: kNoopSessionWriteTxnId,
                lsid: kSessionId,
                startTransaction: true,
                autocommit: false,
            }));

            assert.commandWorked(conn.getDB("admin").runCommand({
                abortTransaction: 1,
                txnNumber: kNoopSessionWriteTxnId,
                lsid: kSessionId,
                autocommit: false,
            }));
        }
    }
];

noOpSessionWrites.forEach(({testDesc, testOp}) => {
    jsTest.log(`Testing no-op session write == ${testDesc} ==.`);
    const tenantMigrationTest =
        new TenantMigrationTest({name: jsTestName(), sharedOptions: {nodes: 1}});

    const donorPrimary = tenantMigrationTest.getDonorPrimary();
    const recipientPrimary = tenantMigrationTest.getRecipientPrimary();

    jsTestLog(`Run no-op session write on recipient prior to migration.`);
    testOp(recipientPrimary);

    // Ensure the in-memory transaction participant on recipient is updated to
    // kNoopSessionWriteTxnId.
    assert.commandFailedWithCode(runRetryableWriteWithTxnIdLessThanNoopWriteTxnId(recipientPrimary),
                                 ErrorCodes.TransactionTooOld);

    jsTestLog("Run retryable write on donor prior to migration.");
    assert.commandWorked(runRetryableWriteWithTxnIdLessThanNoopWriteTxnId(donorPrimary));

    // Migration should succeed.
    TenantMigrationTest.assertCommitted(tenantMigrationTest.runMigration(migrationOpts));

    tenantMigrationTest.stop();
});
