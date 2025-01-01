/**
 * Asserts that for a given log, the oplog_cap_maintainer_thread has
 * not been started and oplog sampling has not occurred.
 */
export function verifyOplogCapMaintainerThreadNotStarted(log) {
    const threadRegex = new RegExp("\"id\":5295000");
    const oplogTruncateMarkersRegex = new RegExp("\"id\":22382");

    assert(!threadRegex.test(log));
    assert(!oplogTruncateMarkersRegex.test(log));
}
