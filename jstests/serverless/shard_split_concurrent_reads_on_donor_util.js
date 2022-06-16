/**
 * This utility file is used to list the different test cases needed for the
 * shard_split_concurrent_reads_on_donor*tests.
 */

'use strict';

function runCommandForConcurrentReadTest(db, cmd, expectedError, isTransaction) {
    const res = db.runCommand(cmd);

    if (expectedError) {
        assert.commandFailedWithCode(res, expectedError, tojson(cmd));
        // The 'TransientTransactionError' label is attached only in a scope of a transaction.
        if (isTransaction &&
            (expectedError == ErrorCodes.TenantMigrationAborted ||
             expectedError == ErrorCodes.TenantMigrationCommitted)) {
            assert(res["errorLabels"] != null, "Error labels are absent from " + tojson(res));
            const expectedErrorLabels = ['TransientTransactionError'];
            assert.sameMembers(res["errorLabels"],
                               expectedErrorLabels,
                               "Error labels " + tojson(res["errorLabels"]) +
                                   " are different from expected " + expectedErrorLabels);
        }
    } else {
        assert.commandWorked(res);
    }

    if (cmd.lsid) {
        assert.commandWorked(db.runCommand({killSessions: [cmd.lsid]}));
    }
}

const shardSplitConcurrentReadTestCases = {
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
        isTransaction: true,
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
        isTransaction: true,
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
    readWithAfterClusterTimeInTxn: {
        isSupportedOnSecondaries: false,
        requiresReadTimestamp: true,
        isTransaction: true,
        command: function(collName, readTimestamp) {
            return {
                find: collName,
                lsid: {id: UUID()},
                txnNumber: NumberLong(0),
                startTransaction: true,
                autocommit: false,
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
