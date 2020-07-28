/**
 * Test that that the donor blocks clusterTime reads that are executed while the migration is in
 * the blocking state but does not block linearizable reads.
 *
 * @tags: [requires_fcv_46, requires_majority_read_concern]
 */

(function() {
'use strict';

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");

const kMaxTimeMS = 5 * 1000;
const kRecipientConnString = "testConnString";
const kConfigDonorsNS = "config.tenantMigrationDonors";

function startMigration(host, dbName, recipientConnString) {
    const primary = new Mongo(host);
    return primary.adminCommand({
        donorStartMigration: 1,
        migrationId: UUID(),
        recipientConnectionString: recipientConnString,
        databasePrefix: dbName,
        readPreference: {mode: "primary"}
    });
}

function runCommand(db, cmd, expectedError) {
    const res = db.runCommand(cmd);
    if (expectedError) {
        assert.commandFailedWithCode(res, expectedError);
    } else {
        assert.commandWorked(res);
    }
}

/**
 * Tests that the donor blocks clusterTime reads in the blocking state with readTimestamp >=
 * blockingTimestamp but does not block linearizable reads.
 */
function testReadCommandWhenMigrationIsInBlocking(rst, testCase, dbName, collName) {
    const primary = rst.getPrimary();

    let blockingFp = configureFailPoint(primary, "pauseTenantMigrationAfterBlockingStarts");
    let migrationThread = new Thread(startMigration, primary.host, dbName, kRecipientConnString);

    // Wait for the migration to enter the blocking state.
    migrationThread.start();
    blockingFp.wait();

    // Wait for the last oplog entry on the primary to be visible in the committed snapshot view of
    // the oplog on all the secondaries to ensure that snapshot reads on the secondaries with
    // unspecified atClusterTime have read timestamp >= blockTimestamp.
    rst.awaitLastOpCommitted();

    const donorDoc = primary.getCollection(kConfigDonorsNS).findOne({databasePrefix: dbName});
    const command = testCase.requiresReadTimestamp
        ? testCase.command(collName, donorDoc.blockTimestamp)
        : testCase.command(collName);
    command.maxTimeMS = kMaxTimeMS;

    const nodes = testCase.isSupportedOnSecondaries ? rst.nodes : [primary];
    nodes.forEach(node => {
        const db = node.getDB(dbName);
        runCommand(db, command, testCase.isLinearizableRead ? null : ErrorCodes.MaxTimeMSExpired);
    });

    blockingFp.off();
    migrationThread.join();
    assert.commandWorked(migrationThread.returnData());
}

const testCases = {
    snapshotReadWithAtClusterTime: {
        isSupportedOnSecondaries: true,
        requiresReadTimestamp: true,
        command: function(collName, readTimestamp) {
            return {
                find: collName,
                readConcern: {
                    level: "snapshot",
                    atClusterTime: readTimestamp,
                }
            };
        },
    },
    snapshotReadWithoutAtClusterTime: {
        isSupportedOnSecondaries: true,
        command: function(collName) {
            return {
                find: collName,
                readConcern: {
                    level: "snapshot",
                }
            };
        },
    },
    snapshotReadWithAtClusterTimeInTxn: {
        isSupportedOnSecondaries: false,
        requiresReadTimestamp: true,
        command: function(collName, readTimestamp) {
            return {
                find: collName,
                lsid: {id: UUID()},
                txnNumber: NumberLong(0),
                startTransaction: true,
                autocommit: false,
                readConcern: {level: "snapshot", atClusterTime: readTimestamp}
            };
        }
    },
    snapshotReadWithoutAtClusterTimeInTxn: {
        isSupportedOnSecondaries: false,
        command: function(collName) {
            return {
                find: collName,
                lsid: {id: UUID()},
                txnNumber: NumberLong(0),
                startTransaction: true,
                autocommit: false,
                readConcern: {level: "snapshot"}
            };
        }
    },
    readWithAfterClusterTime: {
        isSupportedOnSecondaries: true,
        requiresReadTimestamp: true,
        command: function(collName, readTimestamp) {
            return {
                find: collName,
                readConcern: {
                    afterClusterTime: readTimestamp,
                }
            };
        },
    },
    linearizableRead: {
        isSupportedOnSecondaries: false,
        isLinearizableRead: true,
        command: function(collName) {
            return {
                find: collName,
                readConcern: {level: "linearizable"},
            };
        }
    }
};

const rst = new ReplSetTest({nodes: [{}, {rsConfig: {priority: 0}}, {rsConfig: {priority: 0}}]});
rst.startSet();
rst.initiate();

const kCollName = "testColl";

// Run test cases.
const testFuncs = {
    inBlocking: testReadCommandWhenMigrationIsInBlocking,
};

for (const [testName, testFunc] of Object.entries(testFuncs)) {
    for (const [commandName, testCase] of Object.entries(testCases)) {
        let dbName = commandName + "-" + testName + "0";
        testFunc(rst, testCase, dbName, kCollName);
    }
}

rst.stopSet();
})();
