/**
 * Tests the 'featureFlagLockFreeReads' startup setParameter.
 *
 * User set featureFlagLockFreeReads will be overridden to false (disabled) if:
 *   - with enableMajorityReadConcern=false
 * Otherwise, the default for featureFlagLockFreeReads is false.
 *
 * This test is not compatible with the special Lock Free Reads build variant because
 * disableLockFreeReads is overridden there.
 *
 * @tags: [
 *     # This test expects enableMajorityReadConcern to be on by default and cannot run in suites
 *     # that explicitly change that.
 *     requires_majority_read_concern,
 *     requires_replication,
 *     incompatible_with_lockfreereads, // This test is not compatible with special LFR builder
 * ]
 */

(function() {
"use strict";

const replSetName = 'disable_lock_free_reads_server_parameter';

jsTest.log(
    "Starting server with featureFlagLockFreeReads=true in standalone mode: this should turn " +
    "on lock-free reads.");

let conn = MongoRunner.runMongod({setParameter: "featureFlagLockFreeReads=true"});
assert(conn);
checkLog.containsJson(conn, 4788403);  // Logging that lock-free reads is enabled.
MongoRunner.stopMongod(conn);

jsTest.log("Starting server with featureFlagLockFreeReads=false in standalone mode: this is the " +
           "default and nothing should happen.");

conn = MongoRunner.runMongod({setParameter: "featureFlagLockFreeReads=false"});
assert(conn);
assert(!checkLog.checkContainsOnce(conn, "disabling lock-free reads"));
checkLog.containsJson(conn, 4788402);  // Logging that lock-free reads is disabled.
MongoRunner.stopMongod(conn);

jsTest.log(
    "Starting server with featureFlagLockFreeReads=true and enableMajorityReadConcern=false: " +
    "this should override the setting to true.");

conn = MongoRunner.runMongod({
    replSet: replSetName,
    enableMajorityReadConcern: false,
    setParameter: "featureFlagLockFreeReads=true"
});
assert(conn);
checkLog.containsJson(conn, 4788401);  // Logging eMRCf disables lock-free reads.
checkLog.containsJson(conn, 4788402);  // Logging that lock-free reads is disabled.
MongoRunner.stopMongod(conn);

jsTest.log("Starting server as a replica set member with featureFlagLockFreeReads=true: this " +
           "should turn on lock-free reads.");

conn = MongoRunner.runMongod({replSet: replSetName, setParameter: "featureFlagLockFreeReads=true"});
assert(conn);
checkLog.containsJson(conn, 4788403);  // Logging that lock-free reads is enabled.
MongoRunner.stopMongod(conn);

jsTest.log(
    "Starting server as a replica set member with featureFlagLockFreeReads=false: this is the " +
    "default and nothing should happen.");

conn =
    MongoRunner.runMongod({replSet: replSetName, setParameter: "featureFlagLockFreeReads=false"});
assert(conn);
assert(!checkLog.checkContainsOnce(conn, "disabling lock-free reads"));
checkLog.containsJson(conn, 4788402);  // Logging that lock-free reads is disabled.
MongoRunner.stopMongod(conn);
}());
