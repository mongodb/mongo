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
