/**
 * Tests that causal reads are properly blocked or rejected if executed after the migration
 * transitions to the read blocking state.
 *
 * @tags: [requires_fcv_46, requires_majority_read_concern]
 */

(function() {
'use strict';

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();
const primary = rst.getPrimary();

const kDbPrefix = "testPrefix";
const kDbName = kDbPrefix + "0";
const kCollName = "testColl";
const kMaxTimeMS = 3000;

const kRecipientConnString = "testConnString";
const kMigrationId = UUID();

assert.commandWorked(primary.adminCommand({
    donorStartMigration: 1,
    migrationId: kMigrationId,
    recipientConnectionString: kRecipientConnString,
    databasePrefix: kDbPrefix,
    readPreference: {mode: "primary"}
}));

// Wait for the last oplog entry on the primary to be visible in the committed snapshot view of the
// oplog on the secondary to ensure that snapshot reads on the secondary will have read timestamp
// >= blockTimestamp.
rst.awaitLastOpCommitted();

jsTest.log(
    "Test that the donorStartMigration command correctly sets the durable migration state to blocking");

const donorDoc = primary.getDB("config").migrationDonors.findOne();
const oplogEntry = primary.getDB("local").oplog.rs.findOne({ns: "config.migrationDonors", op: "u"});

assert.eq(donorDoc._id, kMigrationId);
assert.eq(donorDoc.databasePrefix, kDbPrefix);
assert.eq(donorDoc.state, "blocking");
assert.eq(donorDoc.blockTimestamp, oplogEntry.ts);

jsTest.log("Test atClusterTime and afterClusterTime reads");

rst.nodes.forEach((node) => {
    assert.commandWorked(node.getDB(kDbName).runCommand({find: kCollName}));

    // Test snapshot reads with and without atClusterTime.
    assert.commandFailedWithCode(node.getDB(kDbName).runCommand({
        find: kCollName,
        readConcern: {
            level: "snapshot",
            atClusterTime: donorDoc.blockTimestamp,
        },
        maxTimeMS: kMaxTimeMS,
    }),
                                 ErrorCodes.MaxTimeMSExpired);

    assert.commandFailedWithCode(node.getDB(kDbName).runCommand({
        find: kCollName,
        readConcern: {
            level: "snapshot",
        },
        maxTimeMS: kMaxTimeMS,
    }),
                                 ErrorCodes.MaxTimeMSExpired);

    // Test read with afterClusterTime.
    assert.commandFailedWithCode(node.getDB(kDbName).runCommand({
        find: kCollName,
        readConcern: {
            afterClusterTime: donorDoc.blockTimestamp,
        },
        maxTimeMS: kMaxTimeMS,
    }),
                                 ErrorCodes.MaxTimeMSExpired);
});

// Test snapshot read with atClusterTime inside transaction.
assert.commandFailedWithCode(primary.getDB(kDbName).runCommand({
    find: kCollName,
    lsid: {id: UUID()},
    txnNumber: NumberLong(0),
    startTransaction: true,
    autocommit: false,
    readConcern: {level: "snapshot", atClusterTime: donorDoc.blockTimestamp},
    maxTimeMS: kMaxTimeMS,
}),
                             ErrorCodes.MaxTimeMSExpired);

// Test snapshot read without atClusterTime inside transaction.
assert.commandFailedWithCode(primary.getDB(kDbName).runCommand({
    find: kCollName,
    lsid: {id: UUID()},
    txnNumber: NumberLong(0),
    startTransaction: true,
    autocommit: false,
    readConcern: {level: "snapshot"},
    maxTimeMS: kMaxTimeMS,
}),
                             ErrorCodes.MaxTimeMSExpired);

jsTest.log("Test linearizable reads");

// Test that linearizable reads are not blocked in the blocking state.
assert.commandWorked(primary.getDB(kDbName).runCommand({
    find: kCollName,
    readConcern: {level: "linearizable"},
    maxTimeMS: kMaxTimeMS,
}));

// TODO (SERVER-49175): Uncomment this test case when committing is handled.
// assert.commandFailedWithCode(primary.getDB(kDbName).runCommand({
//     find: kCollName,
//     readConcern: {level: "linearizable"},
//     maxTimeMS: kMaxTimeMS,
// }),
//                              ErrorCodes.TenantMigrationCommitted);

rst.stopSet();
})();
