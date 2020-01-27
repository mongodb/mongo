/**
 * Retrieves the oldest required timestamp from the serverStatus output.
 *
 * @return {Timestamp} oldest required timestamp for crash recovery.
 */
function getOldestRequiredTimestampForCrashRecovery(database) {
    const res = database.serverStatus().storageEngine;
    const ts = res.oldestRequiredTimestampForCrashRecovery;
    assert(ts instanceof Timestamp,
           'oldestRequiredTimestampForCrashRecovery was not a Timestamp: ' + tojson(res));
    return ts;
}
