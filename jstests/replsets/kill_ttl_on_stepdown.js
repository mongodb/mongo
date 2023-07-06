/**
 * Ensure that a stepdown of the primary kills any TTLMonitor clients, but not the TTLMonitor
 * thread itself and after stepdown finishes, the TTLMonitor resumes.
 *
 * @tags: [requires_replication]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";

const dbName = "kill_ttl_on_stepdown";

const rst = new ReplSetTest({
    nodes: [{}, {rsConfig: {priority: 0}}],
    nodeOptions: {setParameter: "ttlMonitorSleepSecs=15"}
});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();
let db = primary.getDB(dbName);

// Create a TTL index.
db.getCollection("test").createIndex({x: 1}, {expireAfterSeconds: 3600});

function getNumTTLPasses() {
    let serverStatus = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
    return serverStatus.metrics.ttl.passes;
}

// Let the TTLMonitor do some passes.
assert.soon(() => {
    return getNumTTLPasses() > 0;
}, "TTLMonitor never did any passes.");

let failPoint = configureFailPoint(primary, "hangTTLMonitorWithLock");
failPoint.wait();

// See how many passes the TTLMonitor has done, before we stepdown the primary, killing it.
let ttlPassesBeforeStepdown = getNumTTLPasses();

// Force a stepdown of the primary.
assert.commandWorked(
    primary.getDB("admin").runCommand({replSetStepDown: 60 * 10 /* 10 minutes */, force: true}));
rst.waitForState(primary, ReplSetTest.State.SECONDARY);
assert.commandWorked(primary.adminCommand({replSetFreeze: 0}));
assert.commandWorked(primary.adminCommand({replSetStepUp: 1}));

primary = rst.getPrimary();

// Ensure the TTLMonitor was interrupted.
checkLog.contains(primary, "TTLMonitor was interrupted");

// Disable the failpoint on the node that stepped down.
failPoint.off();

// Wait until the number TTLMonitor passes increases, informing us that the TTLMonitor thread
// was not killed entirely and will continue to run after stepdown finishes.
assert.soon(() => {
    if (getNumTTLPasses() > ttlPassesBeforeStepdown) {
        return true;
    }
}, "TTLMonitor was not running after stepdown");

rst.stopSet();