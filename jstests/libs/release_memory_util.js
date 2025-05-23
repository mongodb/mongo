import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

/**
 * Asserts that releaseMemory command failed for a given cursor with the appropriate code.
 */
export function assertReleaseMemoryFailedWithCode(result, cursorId, codes) {
    if (!Array.isArray(codes)) {
        codes = [codes];
    }
    if (!result.hasOwnProperty("cursorsWithErrors")) {
        doassert("Command result does not contain 'cursorsWithErrors' field: " + tojson(result));
    }
    for (const releaseMemoryError of result.cursorsWithErrors) {
        if (releaseMemoryError.cursorId.compare(cursorId) === 0) {
            assert.contains(
                releaseMemoryError.status.code,
                codes,
                "the following release memory error contains an unexpected error code: " +
                    tojson(releaseMemoryError));
            return;
        }
    }
    doassert("Cursor " + tojsononeline(cursorId) +
             " did not fail during release memory. Full command result: " + tojson(result));
}

/**
 * Asserts that releaseMemory command worked for a given cursor.
 */
export function assertReleaseMemoryWorked(result, cursorId) {
    if (!result.hasOwnProperty("cursorsReleased")) {
        doassert("Command result does not contain 'cursorsReleased' field: " + tojson(result));
    }
    for (const releaseMemoryOk of result.cursorsReleased) {
        if (releaseMemoryOk.compare(cursorId) === 0) {
            return;
        }
    }
    doassert("Releasing memory from cursor " + tojsononeline(cursorId) +
             " did not succeed during release memory. Full command result: " + tojson(result));
}

/**
 * Accumulate metric from a server status
 */
export function accumulateServerStatusMetric(db, metricGetter) {
    return retryOnRetryableError(() => {
        let total = 0;
        FixtureHelpers.mapOnEachShardNode({
            db: db,
            func: (db) => {
                const serverStatus = db.serverStatus();
                if (!serverStatus.hasOwnProperty("metrics")) {
                    return;
                }
                total += metricGetter(serverStatus.metrics);
            }
        });
        return total;
    }, 10, 100, [ErrorCodes.InterruptedDueToStorageChange]);
}
