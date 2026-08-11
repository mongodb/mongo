/**
 * Utility functions for random_ddl FSM workloads.
 */

/*
 * Check that an exception has been thrown at least once and log it.
 */
export function checkExceptionHasBeenThrown(
    db,
    exceptionCode,
    logExceptionsDBName,
    logExceptionsCollName,
) {
    const coll = db.getSiblingDB(logExceptionsDBName)[logExceptionsCollName];
    const count = coll.countDocuments({code: exceptionCode});
    const errorName = Object.prototype.hasOwnProperty.call(ErrorCodeStrings, exceptionCode)
        ? ErrorCodeStrings[exceptionCode]
        : exceptionCode;
    assert.gte(count, 1, "No exception has been thrown", {errorName, exceptionCode});
    jsTest.log.info("Thrown exceptions", {count, errorName, exceptionCode});
}

/*
 * Get a random db/coll name from the test lists.
 *
 * Using the thread count to introduce more randomness: it has been observed that concurrent calls to
 * Random.randInt(array.length) are returning too often the same number to different threads.
 */
export function getRandomDbName(threadCount, dbNames) {
    return dbNames[Random.randInt(threadCount * threadCount) % dbNames.length];
}
export function getRandomCollName(threadCount, collNames) {
    return collNames[Random.randInt(threadCount * threadCount) % collNames.length];
}

/**
 * Repeatedly blocks writes on one random shard for a bounded duration. Intended to run as a
 * ParallelTester Thread entry point, independently of FSM workers, so a DDL that is paused by a
 * write block is always eventually unblocked even when every worker is waiting.
 */
export function runWriteBlockToggler(
    shards,
    stopLatch,
    writeBlockReason,
    maxWriteBlockTimeMS,
    unblockedTimeMS,
    randomSeed,
    allowDeletions,
) {
    Random.setRandomSeed(randomSeed);
    const transientContentionCodes = [
        ErrorCodes.LockBusy,
        ErrorCodes.LockTimeout,
        ErrorCodes.ConflictingOperationInProgress,
    ];
    while (stopLatch.getCount() > 0) {
        const shard = shards[Random.randInt(shards.length)];
        const shardAdminDB = new Mongo(shard.host, undefined, {gRPC: false}).getDB("admin");

        jsTest.log.info("Enabling replica set write block", {shard: shard.name});
        assert.soon(() => {
            const res = shardAdminDB.runCommand({
                blockReplicaSetWrites: 1,
                enabled: true,
                allowDeletions: allowDeletions,
                reason: writeBlockReason,
            });
            if (res.ok) {
                return true;
            }
            assert.contains(res.code, transientContentionCodes, () => tojson(res));
            return false;
        }, "Failed to enable replica set write block");
        sleep(maxWriteBlockTimeMS);
        assert.soon(() => {
            const res = shardAdminDB.runCommand({
                blockReplicaSetWrites: 1,
                enabled: false,
                reason: writeBlockReason,
            });
            if (res.ok) {
                return true;
            }
            assert.contains(res.code, transientContentionCodes, () => tojson(res));
            return false;
        }, "Failed to disable replica set write block");
        jsTest.log.info("Disabled replica set write block", {shard: shard.name});

        // Leave writes unblocked for a while before the next block to avoid starving a concurrent
        // DDL operation on the DDL lock.
        sleep(unblockedTimeMS);
    }
}
