/**
 * Checks that the oplog cap maintainer thread doesn't block stepdown.
 *
 * @tags: [requires_replication, requires_persistence]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {waitForState} from "jstests/replsets/rslib.js";

function samplinginProgress(primary) {
    const status = primary.getDB("local").serverStatus();
    assert.commandWorked(status);
    return (
        !status.oplogTruncation.hasOwnProperty("processingMethod") ||
        status.oplogTruncation.processingMethod == "in progress"
    );
}

const rst = new ReplSetTest({
    nodes: 2,
    nodeOptions: {
        setParameter: {
            useSlowCollectionTruncateMarkerScanning: true,
        },
    },
});

rst.startSet();
rst.initiate();

let coll = rst.getPrimary().getDB("test").getCollection("stepdown_test");
for (let i = 0; i < 250; i++) {
    assert.commandWorked(coll.insert({a: i}));
}

// Stop and restart the replica set to re-trigger initial sampling.
rst.stopSet(null, true);
clearRawMongoProgramOutput();
rst.startSet(null, true);
jsTest.log.info("Replica set restarted.");

let primary = rst.getPrimary();
let secondary = rst.getSecondary();

// Verify we're still sampling.
assert(samplinginProgress(primary));
assert(samplinginProgress(secondary));

// Assert we have restarted the cap maintainer thread once.
assert(checkLog.checkContainsWithCountJson(primary, 5295000, {}, /*expectedCount=*/ 1));

// Wait for initial sampling to start
checkLog.containsJson(primary, 11212203);

jsTest.log.info("Attempting to step down primary");
assert.commandWorked(primary.adminCommand({replSetStepDown: 0, force: true}));
waitForState(primary, ReplSetTest.State.SECONDARY);

let interruptCount = assert.commandWorked(primary.getDB("admin").runCommand({serverStatus: 1})).oplogTruncation
    .interruptCount;
assert(interruptCount == 0);

// Assert at this point we haven't completed initial sampling yet.
assert(checkLog.checkContainsWithCountJson(primary, 22382, {}, /*expectedCount=*/ 0));

// Verify we're still sampling.
assert(samplinginProgress(primary));
assert(samplinginProgress(secondary));

interruptCount = assert.commandWorked(primary.getDB("admin").runCommand({serverStatus: 1})).oplogTruncation
    .interruptCount;
assert(interruptCount == 0);

jsTest.log.info("Test complete. Stopping replica set.");
rst.stopSet();

// Make sure shutdown interrupted the OplogCapMaintainerThread.
let subStr = "11212201.*OplogCapMaintainerThread interrupted.*at shutdown";
assert(rawMongoProgramOutput(subStr).search(subStr) !== -1);

// Make sure we have interruption duration metrics at shutdown
subStr = "11211800.*InterruptedAtShutdown.*durationMs";
assert(rawMongoProgramOutput(subStr).search(subStr) !== -1);
