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
 * depending on the value of expectSuccess.
 */
function runCommandAndVerifyResponse(sessionDb, txnNumber, cmdObj, expectSuccess, expectedCode) {
    if (expectSuccess) {
        // A snapshot read may fail with SnapshotTooOld in 4.0 when targeting multiple shards
        // because atClusterTime reads can only be established at the majority commit point, and
        // noop writes may advance the majority commit point past the given atClusterTime
        // resulting in a SnapshotTooOld error. Eventually the read should succeed, when all
        // targeted shards are at the same cluster time, so retry until it does.
        assert.soon(() => {
            const res = sessionDb.runCommand(cmdObj);
            if (!res.ok) {
                assert.commandFailedWithCode(
                    res,
                    ErrorCodes.SnapshotTooOld,
                    "expected command to fail with SnapshotTooOld, cmd: " + tojson(cmdObj));
                print("Retrying because of SnapshotTooOld error.");
                return false;
            }

            assert.commandWorked(res, "expected command to succeed, cmd: " + tojson(cmdObj));
            return true;
        });
    } else {
        assert.commandFailedWithCode(sessionDb.runCommand(cmdObj),
                                     expectedCode,
                                     "command did not fail with expected error code, cmd: " +
                                         tojson(cmdObj) + ", expectedCode: " +
                                         tojson(expectedCode));
    }
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
    runCommandAndVerifyResponse(
        unshardedDb,
        txnNumber,
        {find: "unsharded", readConcern: {level: "snapshot"}, txnNumber: NumberLong(txnNumber++)},
        expectSuccess,
        expectedCode);

    // Sharded collection, one shard.
    const shardedDb = session.getDatabase("shardedDb");
    runCommandAndVerifyResponse(shardedDb,
                                txnNumber,
                                {
                                  find: "sharded",
                                  filter: {x: 1},
                                  readConcern: {level: "snapshot"},
                                  txnNumber: NumberLong(txnNumber++)
                                },
                                expectSuccess,
                                expectedCode);

    // Sharded collection, all shards.
    runCommandAndVerifyResponse(
        shardedDb,
        txnNumber,
        {find: "sharded", readConcern: {level: "snapshot"}, txnNumber: NumberLong(txnNumber++)},
        expectSuccess,
        expectedCode);
}
