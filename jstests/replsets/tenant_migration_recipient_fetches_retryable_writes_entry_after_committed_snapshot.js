/**
 * Tests that the tenant migration recipient correctly reads 'config.transactions' entries from a
 * donor secondary. During secondary oplog application, updates to the same 'config.transactions'
 * entry are coalesced in a single update of the most recent retryable write statement. If the
 * majority committed snapshot of a secondary exists in the middle of a completed batch, then a
 * recipient's majority read on 'config.transactions' can miss committed retryable writes at that
 * majority commit point.
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
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");
load("jstests/libs/fail_point_util.js");  // For configureFailPoint().
load("jstests/libs/uuid_util.js");        // For extractUUIDFromObject().
load("jstests/libs/write_concern_util.js");

const getRecipientCurrOp = function(conn, migrationId) {
    const res = conn.adminCommand({currentOp: true, desc: "tenant recipient migration"});
    assert.eq(res.inprog.length, 1);
    const currOp = res.inprog[0];
    assert.eq(bsonWoCompare(currOp.instanceID, migrationId), 0);

    return currOp;
};

const getDonorSyncSource = function(conn, migrationId) {
    const currOp = getRecipientCurrOp(conn, migrationId);
    return currOp.donorSyncSource;
};

const getStartFetchingDonorOpTime = function(conn, migrationId) {
    const currOp = getRecipientCurrOp(conn, migrationId);
    return currOp.startFetchingDonorOpTime;
};

const oplogApplierBatchSize = 50;

const donorRst = new ReplSetTest({
    nodes: {
        n0: {},
        // Set the 'syncdelay' to 1s to speed up checkpointing. Also explicitly set the batch
        // size for oplog application to ensure the number of retryable write statements being
        // made majority committed isn't a multiple of it.
        n1: {syncdelay: 1, setParameter: {replBatchLimitOperations: oplogApplierBatchSize}},
        // Set the bgSyncOplogFetcherBatchSize to 1 oplog entry to guarantee replication
        // progress with the stopReplProducerOnDocument failpoint.
        n2: {rsConfig: {priority: 0, hidden: true}, setParameter: {bgSyncOplogFetcherBatchSize: 1}},
        n3: {rsConfig: {priority: 0, hidden: true}, setParameter: {bgSyncOplogFetcherBatchSize: 1}},
        n4: {rsConfig: {priority: 0, hidden: true}, setParameter: {bgSyncOplogFetcherBatchSize: 1}},
    },
    // Force secondaries to sync from the primary to guarantee replication progress with the
    // stopReplProducerOnDocument failpoint. Also disable primary catchup because some replicated
    // retryable write statements are intentionally not being made majority committed.
    settings: {chainingAllowed: false, catchUpTimeoutMillis: 0},
    nodeOptions: Object.assign(TenantMigrationUtil.makeX509OptionsForTest().donor, {
        setParameter: {
            tenantMigrationExcludeDonorHostTimeoutMS: 30 * 1000,
            // Allow non-timestamped reads on donor after migration completes for testing.
            'failpoint.tenantMigrationDonorAllowsNonTimestampedReads': tojson({mode: 'alwaysOn'}),
        }
    }),
});
donorRst.startSet();
donorRst.initiateWithHighElectionTimeout();
const donorPrimary = donorRst.getPrimary();

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName(), donorRst: donorRst});

const recipientPrimary = tenantMigrationTest.getRecipientPrimary();
const kTenantId = "testTenantId";
const migrationId = UUID();
const kDbName = tenantMigrationTest.tenantDB(kTenantId, "testDB");
const kCollName = "retryable_write_secondary_oplog_application";
const kNs = `${kDbName}.${kCollName}`;

const migrationOpts = {
    migrationIdString: extractUUIDFromObject(migrationId),
    tenantId: kTenantId,
    // The recipient needs to choose a donor secondary as sync source.
    readPreference: {mode: 'secondary'},
};

const fpAfterConnectingTenantMigrationRecipientInstance = configureFailPoint(
    recipientPrimary, "fpAfterConnectingTenantMigrationRecipientInstance", {action: "hang"});

const fpBeforeWaitingForRetryableWritePreFetchMajorityCommitted = configureFailPoint(
    recipientPrimary, "fpBeforeWaitingForRetryableWritePreFetchMajorityCommitted");

// Start tenant migration and hang after recipient connects to donor sync source.
jsTestLog(`Starting tenant migration: ${tojson(migrationOpts)}`);
assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));
fpAfterConnectingTenantMigrationRecipientInstance.wait();

// Recipient should connect to secondary1 as other secondaries are hidden.
const [secondary1, secondary2, secondary3, secondary4] = donorRst.getSecondaries();
const syncSourceSecondaryHost = getDonorSyncSource(recipientPrimary, migrationId);
assert.eq(syncSourceSecondaryHost, secondary1.host);

assert.commandWorked(
    donorPrimary.getCollection(kNs).insert({_id: 0, counter: 0}, {writeConcern: {w: 5}}));

// The default WC is majority and the donor replica set can't satisfy majority writes after we
// stop replication on the secondaries.
assert.commandWorked(donorPrimary.adminCommand(
    {setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}));
donorRst.awaitReplication();

// Disable replication on all of the secondaries to manually control the replication progress.
const stopReplProducerFailpoints = [secondary1, secondary2, secondary3, secondary4].map(
    conn => configureFailPoint(conn, 'stopReplProducer'));

// While replication is still entirely disabled, additionally disable replication partway
// into the retryable write on other secondaries. The idea is that while secondary1 will
// apply all of the oplog entries in a single batch, other secondaries will only apply up
// to counterMajorityCommitted oplog entries.
const counterTotal = oplogApplierBatchSize;
const counterMajorityCommitted = counterTotal - 2;
jsTestLog(`counterTotal: ${counterTotal}, counterMajorityCommitted: ${counterMajorityCommitted}`);
const stopReplProducerOnDocumentFailpoints = [secondary2, secondary3, secondary4].map(
    conn => configureFailPoint(conn,
                               'stopReplProducerOnDocument',
                               {document: {"diff.u.counter": counterMajorityCommitted + 1}}));

// Perform all the retryable write statements on donor primary.
const lsid = ({id: UUID()});
assert.commandWorked(donorPrimary.getCollection(kNs).runCommand("update", {
    updates: Array.from({length: counterTotal}, () => ({q: {_id: 0}, u: {$inc: {counter: 1}}})),
    lsid,
    txnNumber: NumberLong(1),
}));

// Get the majority committed and last oplog entry of the respective retryable write statements.
const stmtTotal =
    donorPrimary.getCollection("local.oplog.rs").findOne({"o.diff.u.counter": counterTotal});
const stmtMajorityCommitted = donorPrimary.getCollection("local.oplog.rs").findOne({
    "o.diff.u.counter": counterMajorityCommitted
});

assert.neq(null, stmtTotal);
assert.neq(null, stmtMajorityCommitted);
jsTestLog(`stmtTotal timestamp: ${tojson(stmtTotal.ts)}`);
jsTestLog(`stmtMajorityCommitted timestamp: ${tojson(stmtMajorityCommitted.ts)}`);

for (const fp of stopReplProducerFailpoints) {
    fp.off();
    // Wait for secondary1 to have applied through the counterTotal retryable write statement and
    // other secondaries applied through the counterMajorityCommitted retryable write statement,
    // to guarantee that secondary1 will advance its stable_timestamp when learning of the other
    // secondaries also having applied through counterMajorityCommitted.
    assert.soon(() => {
        const {optimes: {appliedOpTime, durableOpTime}} =
            assert.commandWorked(fp.conn.adminCommand({replSetGetStatus: 1}));

        print(`${fp.conn.host}: ${tojsononeline({
            appliedOpTime,
            durableOpTime,
            stmtMajorityCommittedTimestamp: stmtMajorityCommitted.ts
        })}`);

        const stmtTarget = (fp.conn.host === secondary1.host) ? stmtTotal : stmtMajorityCommitted;

        return bsonWoCompare(appliedOpTime.ts, stmtTarget.ts) >= 0 &&
            bsonWoCompare(durableOpTime.ts, stmtTarget.ts) >= 0;
    });
}

// Wait for secondary1 to have advanced its stable timestamp, and therefore updated the
// committed snapshot.
assert.soon(() => {
    const {lastStableRecoveryTimestamp} =
        assert.commandWorked(secondary1.adminCommand({replSetGetStatus: 1}));

    print(`${secondary1.host}: ${tojsononeline({
        lastStableRecoveryTimestamp,
        stmtMajorityCommittedTimestamp: stmtMajorityCommitted.ts
    })}`);

    return bsonWoCompare(lastStableRecoveryTimestamp, stmtMajorityCommitted.ts) >= 0;
});

// Wait before tenant migration starts to wait for the retryable write pre-fetch result to be
// majority committed.
fpAfterConnectingTenantMigrationRecipientInstance.off();
fpBeforeWaitingForRetryableWritePreFetchMajorityCommitted.wait();

const startFetchingDonorOpTime = getStartFetchingDonorOpTime(recipientPrimary, migrationId);
assert.eq(startFetchingDonorOpTime.ts, stmtMajorityCommitted.ts);

// At this point, the recipient should have fetched retryable writes and put them into the
// oplog buffer.
const kOplogBufferNS = `config.repl.migration.oplog_${migrationOpts.migrationIdString}`;
const recipientOplogBuffer = recipientPrimary.getCollection(kOplogBufferNS);
jsTestLog(`oplog buffer ns: ${kOplogBufferNS}`);

// Number of entries fetched into oplog buffer is the majority committed count - 1 since we only
// fetch entries that occur before startFetchingDonorOpTime, which is equal to the commit point.
const findRes = recipientOplogBuffer.find().toArray();
const expectedCount = counterMajorityCommitted - 1;
assert.eq(
    findRes.length, expectedCount, `Incorrect number of oplog buffer entries: ${tojson(findRes)}`);

// Resume replication on all the secondaries and wait for migration to complete.
for (const fp of stopReplProducerOnDocumentFailpoints) {
    fp.off();
}

fpBeforeWaitingForRetryableWritePreFetchMajorityCommitted.off();

TenantMigrationTest.assertCommitted(tenantMigrationTest.waitForMigrationToComplete(migrationOpts));
assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));

// After migration, verify that if we perform the same retryable write statements on the recipient,
// these statements will not be executed.
let docAfterMigration = recipientPrimary.getCollection(kNs).findOne({_id: 0});
assert.eq(docAfterMigration.counter, counterTotal);

assert.commandWorked(recipientPrimary.getCollection(kNs).runCommand("update", {
    updates: Array.from({length: counterTotal}, () => ({q: {_id: 0}, u: {$inc: {counter: 1}}})),
    lsid,
    txnNumber: NumberLong(1),
}));

// The second query should return the same result as first one, since the recipient should have
// fetched the executed retryable writes from donor.
docAfterMigration = recipientPrimary.getCollection(kNs).findOne({_id: 0});
assert.eq(docAfterMigration.counter, counterTotal);

donorRst.stopSet();
tenantMigrationTest.stop();
})();
