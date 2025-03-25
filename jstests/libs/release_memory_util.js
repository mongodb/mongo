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
