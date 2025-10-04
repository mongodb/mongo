/*
 * This test performs a read using linearizable read concern that will run in the end phase of step
 * up where the new primary is set to primary but is not yet accepting writes.
 * The linearizable read should not crash the server, but should hang, after the read, before the
 * write -- linearizable does a read then no-op write
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

let sendLinearizableReadOnFailpoint = function () {
    // Linearizable read concern is not allowed on secondaries. But set this flag so we can start
    // the operation during the transition from secondary to primary. The read sent at this state
    // should be handled gracefully with appropriate response from the server and not crash.
    db.getMongo().setSecondaryOk();

    let coll = db.getSiblingDB("test").foo;

    try {
        jsTestLog(
            "Waiting for new primary to reach drain complete state where the node is primary but is not yet accepting writes",
        );
        assert.commandWorked(
            db.getMongo().adminCommand({
                waitForFailPoint: "hangAfterReconfigOnDrainComplete",
                timesEntered: 1,
                maxTimeMS: 5 * 60 * 1000,
            }),
        );

        jsTestLog("Sending in linearizable read in secondary thread");

        // In lock free reads this will error with NotWritablePrimary. Without lock free reads we
        // timeout because we can't acquire the RSTL.
        assert.commandFailedWithCode(
            coll.runCommand({"find": "foo", readConcern: {level: "linearizable"}, maxTimeMS: 10000}),
            [ErrorCodes.NotWritablePrimary, ErrorCodes.MaxTimeMSExpired],
        );
    } finally {
        // Turn off fail point so we can cleanup.
        assert.commandWorked(
            db.getMongo().adminCommand({configureFailPoint: "hangAfterReconfigOnDrainComplete", mode: "off"}),
        );
    }
};

let num_nodes = 3;
let name = "stepup_with_linearizable_read";
let replTest = new ReplSetTest({name: name, nodes: num_nodes});
let config = replTest.getReplSetConfig();

// Increased election timeout to avoid having unrelated primary step down while we are
// testing linearizable functionality on a specific node.
config.settings = {
    electionTimeoutMillis: 60000,
};

replTest.startSet();
replTest.initiate(config);
replTest.awaitReplication();

let primary = replTest.getPrimary();
let secondaries = replTest.getSecondaries();

// Do a write to have something to read, make sure it is replicated to all nodes so the step up will
// succeed.
assert.commandWorked(
    primary
        .getDB("test")
        .foo.insert({"number": 7}, {"writeConcern": {"w": num_nodes, "wtimeout": ReplSetTest.kDefaultTimeoutMS}}),
);

let newPrimary = secondaries[0];

jsTestLog("Set failpoint so we hang during stepup when drain mode is complete but before we are writable.");

assert.commandWorked(
    newPrimary.adminCommand({configureFailPoint: "hangAfterReconfigOnDrainComplete", mode: "alwaysOn"}),
);

jsTestLog("Starting parallel reader");

let parallelShell = startParallelShell(sendLinearizableReadOnFailpoint, newPrimary.port);

jsTestLog(
    "Stepping up secondary, which will hang before step-up completion so that the parallel linearizable read can run, and then it will finish.",
);

// Stepping up to be new primary. This will not finish until the fail point is turned off in the
// parallel shell.
assert.commandWorked(newPrimary.adminCommand({"replSetStepUp": 1}));

// Wait for the parallel shell to finish.
parallelShell();

replTest.stopSet();
