/**
 * Helper functions for testing global snapshot reads in the multiversion suite.
 */

function supportsSnapshotReadConcern() {
    const conn = MongoRunner.runMongod();
    const supportsSnapshotReadConcern =
        conn.getDB("test").serverStatus().storageEngine.supportsSnapshotReadConcern;
    MongoRunner.stopMongod(conn);

    return supportsSnapshotReadConcern;
}

/**
 * Runs the given command on the given database, asserting the command failed or succeeded
 * depending on the value of expectSuccess. Returns the last used txnNumber.
 */
function runCommandAndVerifyResponse(sessionDb, txnNumber, cmdObj, expectSuccess, expectedCode) {
    if (expectSuccess) {
        // A snapshot read may fail with SnapshotTooOld in 4.0 when targeting multiple shards
        // because atClusterTime reads can only be established at the majority commit point, and
        // noop writes may advance the majority commit point past the given atClusterTime
        // resulting in a SnapshotTooOld error. Eventually the read should succeed, when all
        // targeted shards are at the same cluster time, so retry until it does.
        // A snapshot read may also fail with NoSuchTransaction if it encountered a StaleEpoch
        // error while it was running.
        assert.soon(() => {
            const res = sessionDb.runCommand(cmdObj);
            if (!res.ok) {
                assert(res.code === ErrorCodes.SnapshotTooOld ||
                           res.code === ErrorCodes.NoSuchTransaction,
                       "expected command to fail with SnapshotTooOld or NoSuchTransaction, cmd: " +
                           tojson(cmdObj) + ", result: " + tojson(res));
                print("Retrying because of SnapshotTooOld or NoSuchTransaction error.");
                txnNumber++;
                cmdObj.txnNumber = NumberLong(txnNumber);
                return false;
            }

            assert.commandWorked(res, "expected command to succeed, cmd: " + tojson(cmdObj));
            return true;
        });
    } else {
        assert.commandFailedWithCode(
            sessionDb.runCommand(cmdObj),
            expectedCode,
            "command did not fail with expected error code, cmd: " + tojson(cmdObj) +
                ", expectedCode: " + tojson(expectedCode));
    }
    return txnNumber;
}

/**
 * Runs reads with snapshot readConcern against mongos, expecting they either fail or succeed
 * depending on the expectSuccess parameter.
 */
function verifyGlobalSnapshotReads(conn, expectSuccess, expectedCode) {
    const session = conn.startSession({causalConsistency: false});
    let txnNumber = 0;  // Counter used and incremented for all snapshot reads per session.

    // Unsharded collection.
    const unshardedDb = session.getDatabase("unshardedDb");
    txnNumber = runCommandAndVerifyResponse(
        unshardedDb,
        txnNumber,
        {find: "unsharded", readConcern: {level: "snapshot"}, txnNumber: NumberLong(txnNumber)},
        expectSuccess,
        expectedCode);

    // Sharded collection, one shard.
    txnNumber++;
    const shardedDb = session.getDatabase("shardedDb");
    txnNumber = runCommandAndVerifyResponse(shardedDb,
                                            txnNumber,
                                            {
                                                find: "sharded",
                                                filter: {x: 1},
                                                readConcern: {level: "snapshot"},
                                                txnNumber: NumberLong(txnNumber)
                                            },
                                            expectSuccess,
                                            expectedCode);

    // TODO: SERVER-31767
    const server_31767_fixed = false;
    if (server_31767_fixed) {
        // Sharded collection, all shards.
        txnNumber++;
        txnNumber = runCommandAndVerifyResponse(
            shardedDb,
            txnNumber,
            {find: "sharded", readConcern: {level: "snapshot"}, txnNumber: NumberLong(txnNumber)},
            expectSuccess,
            expectedCode);
    }
}
