/**
 * Tests the 'disableLockFreeReads' startup setParameter.
 *
 * User set disableLockFreeReads will be overridden to true (disabled) if:
 *   - in standalone mode
 *   - with enableMajorityReadConcern=false
 * Otherwise, the default for disableLockFreeReads is true.
 *
 * @tags: [
 *     # This test expects enableMajorityReadConcern to be on by default and cannot run in suites
 *     # that explicitly change that.
 *     requires_majority_read_concern,
 *     requires_replication,
 * ]
 */

(function() {
"use strict";

const replSetName = 'disable_lock_free_reads_server_parameter';

jsTest.log("Starting server with disableLockFreeReads=false in standalone mode: this should " +
           "override the setting to true.");

let conn = MongoRunner.runMongod({setParameter: "disableLockFreeReads=false"});
assert.neq(conn, null);
checkLog.contains(conn, "disabling lock-free reads");
checkLog.contains(conn, "Lock-free reads is disabled");
MongoRunner.stopMongod(conn);

jsTest.log("Starting server with disableLockFreeReads=false and enableMajorityReadConcern=false: " +
           "this should override the setting to true.");

conn = MongoRunner.runMongod({
    replSet: replSetName,
    enableMajorityReadConcern: false,
    setParameter: "disableLockFreeReads=false"
});
assert.neq(conn, null);
checkLog.contains(conn, "disabling lock-free reads");
checkLog.contains(conn, "Lock-free reads is disabled");
MongoRunner.stopMongod(conn);

jsTest.log("Starting server in standalone mode with disableLockFreeReads=true: this is the " +
           "default and nothing should happen.");

conn = MongoRunner.runMongod({setParameter: "disableLockFreeReads=true"});
assert.neq(conn, null);
assert(!checkLog.checkContainsOnce(conn, "disabling lock-free reads"));
checkLog.contains(conn, "Lock-free reads is disabled");
MongoRunner.stopMongod(conn);

jsTest.log("Starting server as a replica set member with disableLockFreeReads=false: this should " +
           "turn on lock-free reads.");

conn = MongoRunner.runMongod({replSet: replSetName, setParameter: "disableLockFreeReads=false"});
assert.neq(conn, null);
checkLog.contains(conn, "Lock-free reads is enabled");
MongoRunner.stopMongod(conn);

jsTest.log("Starting server as a replica set member with disableLockFreeReads=true: this is the " +
           "default and nothing should happen.");

conn = MongoRunner.runMongod({replSet: replSetName, setParameter: "disableLockFreeReads=true"});
assert.neq(conn, null);
assert(!checkLog.checkContainsOnce(conn, "disabling lock-free reads"));
checkLog.contains(conn, "Lock-free reads is disabled");
MongoRunner.stopMongod(conn);
}());
