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
 *     retried. Receives the received exception as an input an is supposed to return true for
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
