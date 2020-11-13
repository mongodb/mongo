/**
 * Tests the 'disableLockFreeReads' startup setParameter.
 *
 * User set disableLockFreeReads will be overridden to true (disabled) if:
 *   - with enableMajorityReadConcern=false
 * Otherwise, the default for disableLockFreeReads is true.
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

jsTest.log("Starting server with disableLockFreeReads=false in standalone mode: this should turn " +
           "on lock-free reads.");

let conn = MongoRunner.runMongod({setParameter: "disableLockFreeReads=false"});
assert(conn);
checkLog.containsJson(conn, 4788403);  // Logging that lock-free reads is enabled.
MongoRunner.stopMongod(conn);

jsTest.log("Starting server with disableLockFreeReads=true in standalone mode: this is the " +
           "default and nothing should happen.");

conn = MongoRunner.runMongod({setParameter: "disableLockFreeReads=true"});
assert(conn);
assert(!checkLog.checkContainsOnce(conn, "disabling lock-free reads"));
checkLog.containsJson(conn, 4788402);  // Logging that lock-free reads is disabled.
MongoRunner.stopMongod(conn);

jsTest.log("Starting server with disableLockFreeReads=false and enableMajorityReadConcern=false: " +
           "this should override the setting to true.");

conn = MongoRunner.runMongod({
    replSet: replSetName,
    enableMajorityReadConcern: false,
    setParameter: "disableLockFreeReads=false"
});
assert(conn);
checkLog.containsJson(conn, 4788401);  // Logging eMRCf disables lock-free reads.
checkLog.containsJson(conn, 4788402);  // Logging that lock-free reads is disabled.
MongoRunner.stopMongod(conn);

jsTest.log("Starting server as a replica set member with disableLockFreeReads=false: this should " +
           "turn on lock-free reads.");

conn = MongoRunner.runMongod({replSet: replSetName, setParameter: "disableLockFreeReads=false"});
assert(conn);
checkLog.containsJson(conn, 4788403);  // Logging that lock-free reads is enabled.
MongoRunner.stopMongod(conn);

jsTest.log("Starting server as a replica set member with disableLockFreeReads=true: this is the " +
           "default and nothing should happen.");

conn = MongoRunner.runMongod({replSet: replSetName, setParameter: "disableLockFreeReads=true"});
assert(conn);
assert(!checkLog.checkContainsOnce(conn, "disabling lock-free reads"));
checkLog.containsJson(conn, 4788402);  // Logging that lock-free reads is disabled.
MongoRunner.stopMongod(conn);
}());
