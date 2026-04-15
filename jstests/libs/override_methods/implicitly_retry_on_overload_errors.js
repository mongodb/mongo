/**
 * Overrides runCommand to retry operations that encounter overload errors (e.g. rate limiting).
 * This override checks for the RetryableError error label and automatically retries with
 * exponential backoff.
 *
 * This is used in passthrough suites that enable the ingress request rate limiter failpoint
 * to test that clients can handle rate limiting errors gracefully.
 */

import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";

// Default configuration values that can be overridden via TestData.
const kDefaultMaxRetries = TestData.rateLimitRetryMaxRetries || 16;
const kDefaultBaseBackoffMs = TestData.rateLimitRetryBaseBackoffMs || 10;
const kDefaultMaxBackoffMs = TestData.rateLimitRetryMaxBackoffMs || 1000;

const kRetryableErrorLabel = "RetryableError";
const kSystemOverloadedErrorLabel = "SystemOverloadedError";

function hasErrorLabel(res, label) {
    if (!res || typeof res !== "object") {
        return false;
    }

    if (!res.hasOwnProperty("errorLabels") || !Array.isArray(res.errorLabels)) {
        return false;
    }

    return res.errorLabels.includes(label);
}

function hasRetryableErrorLabel(res) {
    return hasErrorLabel(res, kRetryableErrorLabel);
}

function hasSystemOverloadedErrorLabel(res) {
    return hasErrorLabel(res, kSystemOverloadedErrorLabel);
}

/**
 * Calculates the backoff delay for a given retry attempt using exponential backoff with jitter.
 */
function calculateBackoffMs(attempt, baseBackoffMs, maxBackoffMs) {
    // Exponential backoff: base * 2^attempt
    const exponentialBackoff = baseBackoffMs * Math.pow(2, attempt);
    // Cap at max backoff.
    const cappedBackoff = Math.min(exponentialBackoff, maxBackoffMs);
    // Add jitter: uniform distribution between 0 and cappedBackoff.
    // The +1 makes the upper bound inclusive.
    return Math.floor(Math.random() * (cappedBackoff + 1));
}

/**
 * Runs the command with automatic retries on retryable errors.
 */
function runCommandWithOverloadRetries(conn, dbName, cmdName, cmdObj, func, makeFuncArgs) {
    let res;
    let attempt = 0;

    while (attempt <= kDefaultMaxRetries) {
        res = func.apply(conn, makeFuncArgs(cmdObj));

        // If the command succeeded or the error is not retryable, return immediately.
        // The server will not apply the RetryableError label if the command cannot be retried
        // (e.g., in transactions, or for certain command types like getMore).
        if (res.ok === 1 || !hasRetryableErrorLabel(res)) {
            return res;
        }

        attempt++;

        if (attempt > kDefaultMaxRetries) {
            jsTest.log.info(
                `Overload retry override: Exhausted all ${kDefaultMaxRetries} retries for command '${cmdName}'`,
                {command: cmdObj, response: res},
            );
            break;
        }

        // Only backoff if the system is overloaded.
        if (hasSystemOverloadedErrorLabel(res)) {
            const backoffMs = calculateBackoffMs(attempt, kDefaultBaseBackoffMs, kDefaultMaxBackoffMs);

            jsTest.log.info(
                `Overload retry override: Retrying command '${cmdName}' after overload error. ` +
                    `Attempt ${attempt}/${kDefaultMaxRetries}, backoff ${backoffMs}ms`,
                {command: cmdObj, response: res},
            );

            sleep(backoffMs);
        } else {
            jsTest.log.info(
                `Overload retry override: Retrying command '${cmdName}' after retryable error. ` +
                    `Attempt ${attempt}/${kDefaultMaxRetries}, no backoff`,
                {command: cmdObj, response: res},
            );
        }
    }

    return res;
}

OverrideHelpers.prependOverrideInParallelShell("jstests/libs/override_methods/implicitly_retry_on_overload_errors.js");

OverrideHelpers.overrideRunCommand(runCommandWithOverloadRetries);
