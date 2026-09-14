/**
 * Helper function for retrying execution of a callback function until success or a terminal error
 * condition.
 */

/**
 * Executes the callback function 'fn' until it succeeds (i.e. does not throw an exception), the
 * maximum number of retry attempts is reached, or a terminal error condition is reached. Retries
 * are executed using an expotentiall backoff. The maximum number of retries is configurable.
 *
 * @param {function} fn the callback to execute
 * @param {function} shouldRetry a function to determine whether or not the execution should be
 *     retried. Receives a caught exception as an input and is supposed to return true for
 *     another retry attempt, and false otherwise.
 * @param {number} maxRetries maximum number of attempts (including the first attempt).
 * @param {number} initialBackoffMs initial value for expoential backoff in milliseconds.
 *
 * @return {any} the result of the callback function 'fn' on success, and an exception otherwise.
 */
export function runWithRetries(fn, shouldRetry, maxRetries = 3, initialBackoffMs = 100) {
    let backoff = initialBackoffMs;
    for (let attemptNumber = 1; attemptNumber <= maxRetries; attemptNumber++) {
        try {
            return fn();
        } catch (e) {
            jsTest.log(`runWithRetries: attempt ${attemptNumber} failed with error: ${tojson(e)}`);
            if (attemptNumber === maxRetries || !shouldRetry(e)) {
                throw e;
            }
            sleep(backoff);
            backoff *= 2;
        }
    }

    // Cannot get here.
    throw new Error("runWithRetries: unreachable error");
}

/**
 * Executes the callback function 'fn', retrying if it fails with InterruptedDueToStorageChange in
 * addition to the usual retryable errors. A node performing a file copy based initial sync kills
 * in-flight commands with that code, which belongs to the Interruption and CancellationError
 * categories but not to RetriableError, so no other command helper retries it.
 *
 * @param {function} fn the callback to execute
 * @param {number} numRetries number of retries allowed after the first attempt
 * @param {number} sleepMs sleep time in milliseconds between retries
 *
 * @return {any} the result of the callback function 'fn' on success.
 */
export function retryOnStorageChangeInterrupt(fn, numRetries = 10, sleepMs = 100) {
    return retryOnRetryableError(fn, numRetries, sleepMs, [
        ErrorCodes.InterruptedDueToStorageChange,
    ]);
}
