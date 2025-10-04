/* SERVER-56756 Ensure that during a stepDown operation we fassert when
 * getting the RSTL takes longer than fassertOnLockTimeoutForStepUpDown seconds
 *
 * 1. Start up a 3 node replica set
 * 2. Enable failpoint hangWithLockDuringBatchInsert
 * 3. Do one write in the background which, due to the failpoint, will hang
 *    after getting locks including RSTL
 * 4. Send in the background request to the PRIMARY to StepDown.
 *    stepDown thread will timeout since it cant get RSTL, and will catch
 *    ErrorCodes::LockTimeout and caus an fassert (node crash) on the primary
 *    [since deadline is beyond the RSTL fassert timeout].
 * 5. Make sure that primary node is down and that another has stepedUp
 *
 * @tags: [
 *   requires_fcv_53,
 *   featureFlagIntentRegistration_incompatible,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

// Because this test intentionally crashes the server via an fassert, we need to instruct the
// shell to clean up the core dump that is left behind.
TestData.cleanUpCoreDumpsFromExpectedCrash = true;

let name = "interruptStepDown";
// Set the fassert timeout to shorter than the default to avoid having a long-running test.
let replSet = new ReplSetTest({
    name: name,
    nodes: 3,
    nodeOptions: {setParameter: "fassertOnLockTimeoutForStepUpDown=5"},
});
let nodes = replSet.nodeList();
replSet.startSet();
replSet.initiate(
    {
        "_id": name,
        "members": [
            {"_id": 0, "host": nodes[0]},
            {"_id": 1, "host": nodes[1]},
            {"_id": 2, "host": nodes[2], "priority": 0},
        ],
    },
    null,
    {initiateWithDefaultElectionTimeout: true},
);

replSet.waitForState(replSet.nodes[0], ReplSetTest.State.PRIMARY);

let primary = replSet.getPrimary();
let secondary = replSet.getSecondary();
assert.eq(primary.host, nodes[0], "primary assumed to be node 0");
assert.commandWorked(
    primary.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}),
);
replSet.awaitReplication();

// Makes sure writes work
assert.commandWorked(primary.getDB(name).foo.insert([{_id: 0}, {_id: 1}, {_id: 2}]));

// Enable failpoint which waits after getting locks
let failPoint = configureFailPoint(primary.getDB(name), "hangWithLockDuringBatchInsert", {
    shouldCheckForInterrupt: false,
    shouldContinueOnInterrupt: false,
});

jsTestLog("Initiating write which will hang on failpoint");
let bgInserter = startParallelShell(
    "db.getSiblingDB('interruptStepDown').foo.insert([{_id:0}, {_id:1}, {_id:2}]);",
    primary.port,
);

jsTestLog("Wait for failpoint after bgwrite and before asking the PRIMARY to stepdown");
failPoint.wait();

let stepDownCmd = function () {
    jsTestLog("Sending stepdown to primary");
    db.getSiblingDB("admin").runCommand({replSetStepDown: 10, force: true});
};
let stepDowner = startParallelShell(stepDownCmd, primary.port);

jsTestLog("Waiting for primary to be down");
replSet.waitForState(primary, ReplSetTest.State.DOWN);

jsTestLog("Make sure there is a new primary");
let newprimary = replSet.getPrimary();
assert(primary != newprimary);

stepDowner({checkExitSuccess: false});
bgInserter({checkExitSuccess: false});

// We expect primary to have crashed with an fassert.
replSet.stop(primary.nodeId, undefined, {forRestart: false, allowedExitCode: MongoRunner.EXIT_ABORT});
replSet.stopSet();
