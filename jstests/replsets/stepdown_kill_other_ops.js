// SERVER-15310 Ensure that stepDown kills all other running operations

import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const replSet = new ReplSetTest({name: TestData.name, nodes: 2});
const nodes = replSet.nodeList();
replSet.startSet();
replSet.initiate();

const primary = replSet.getPrimary();
assert.eq(primary.host, nodes[0], "primary assumed to be node 0");

// Get the initial topology version.
const res = assert.commandWorked(primary.adminCommand({hello: 1}));
assert(res.hasOwnProperty("topologyVersion"), tojson(res));
const topologyVersionField = res.topologyVersion;

// Run sleep in a separate thread to take the global write lock which would prevent stepdown
// from completing if it failed to kill all running operations.
jsTestLog("Running {sleep:1, lock: 'w'} to grab global write lock");
const sleepCmd = function (topologyVersionField) {
    // Run for 10 minutes if not interrupted.
    let res = assert.commandFailedWithCode(
        db.adminCommand({sleep: 1, lock: "w", seconds: 60 * 10}),
        ErrorCodes.InterruptedDueToReplSetStateChange,
    );
    // Topology version should have been incremented.
    assert.gt(
        res.topologyVersion.counter,
        topologyVersionField.counter,
        "Command was interrupted but topology version not incremented.",
    );
};
const startTime = new Date().getTime() / 1000;
const sleepRunner = startParallelShell(funWithArgs(sleepCmd, topologyVersionField), primary.port);

jsTestLog("Confirming that sleep() is running and has the global lock");
assert.soon(function () {
    const res = primary.getDB("admin").currentOp();
    for (let index in res.inprog) {
        const entry = res.inprog[index];
        if (entry["command"] && entry["command"]["sleep"]) {
            if ("W" === entry["locks"]["Global"]) {
                return true;
            }
        }
    }
    return false;
}, "sleep never ran and grabbed the global write lock");

jsTestLog("Stepping down");
assert.commandWorked(primary.getDB("admin").runCommand({replSetStepDown: 30}));

jsTestLog("Waiting for former PRIMARY to become SECONDARY");
replSet.awaitSecondaryNodes(30000, [primary]);

const newPrimary = replSet.getPrimary();
assert.neq(primary, newPrimary, "SECONDARY did not become PRIMARY");

sleepRunner({checkExitSuccess: false});
const endTime = new Date().getTime() / 1000;
const duration = endTime - startTime;
assert.lt(
    duration,
    60 * 9, // In practice, this should be well under 1 minute.
    "Sleep lock held longer than expected, possibly uninterrupted.",
);

replSet.stopSet();
