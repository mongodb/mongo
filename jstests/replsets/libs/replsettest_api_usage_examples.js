import {ReplSetTest} from "jstests/libs/replsettest.js";

function nodeWithHighestPriorityStepsUp() {
    // Test replica set with different priorities.
    const replTest = new ReplSetTest({
        name: "NodesWithDifferentPriorities",
        nodes: [{rsConfig: {priority: 5}}, {}, {rsConfig: {priority: 0}}]
    });

    replTest.startSet();
    replTest.initiate(null, null, {initiateWithDefaultElectionTimeout: true});

    // Make sure that node[0] is the primary because it has the highest priority.
    replTest.waitForState(replTest.nodes[0], ReplSetTest.State.PRIMARY);

    // Stop the primary and wait for another node to become primary. Node 1 will become primary
    // because the third node has priority 0 so it cannot become primary and cannot trigger
    // elections.
    const primary = replTest.getPrimary();
    replTest.stop(primary, undefined /* signal */, {} /* options */, {forRestart: true});
    replTest.waitForState(replTest.nodes[1], ReplSetTest.State.PRIMARY);

    // Start the old primary again and make sure it becomes primary again due to priority takeover.
    // Calling `stop()` and `start()` with `restart=true` will skip clearing the data directory
    // before the server starts.
    replTest.start(primary, {} /* options */, true /* restart */);
    replTest.waitForState(primary, ReplSetTest.State.PRIMARY);

    // Shut down the set and finish the test.
    replTest.stopSet();
}

function manuallyStepUpNodeWhenHighElectionTimeoutSet() {
    const replTest = new ReplSetTest({name: "highElectionTimeoutAndStepUp", nodes: 3});

    replTest.startSet();

    // Call `initiate` without `initiateWithDefaultElectionTimeout: true` when not testing election
    // behavior. This initiates the replica set with high election timeout, which prevents unplanned
    // elections from happening during the test, so we can maintain the same primary throughout the
    // test.
    replTest.initiate();

    // Test some behavior.
    const primary = replTest.getPrimary();
    const primaryDB = primary.getDB("db");
    assert.commandWorked(primaryDB["collection"].insertMany(
        [...Array(100).keys()].map(x => ({a: x.toString()})), {ordered: false}));

    // Call `stepUp()` to make another node the primary. This waits for all nodes to reach the same
    // optime before sending the replSetStepUp command to the node, so that the stepup command will
    // not fail because the candidate is too stale. It also waits for the new primary to become a
    // writable primary.
    const newPrimary = replTest.getSecondary();
    replTest.stepUp(newPrimary);
    replTest.waitForState(newPrimary, ReplSetTest.State.PRIMARY);

    // Shut down the set and finish the test.
    replTest.stopSet();
}

function frozenNodesDontTriggerElections() {
    const replTest = new ReplSetTest({
        name: "freezeNode",
        nodes: [{rsConfig: {priority: 5}}, {rsConfig: {priority: 3}}, {rsConfig: {priority: 0}}]
    });
    replTest.startSet();
    replTest.initiate(null, null, {initiateWithDefaultElectionTimeout: true});

    // Step down the primary and freeze it for 30 seconds. Freezing a node prevents it from
    // attempting to become primary.
    const primary = replTest.getPrimary();
    replTest.awaitReplication();
    assert.commandWorked(primary.getDB("admin").runCommand({replSetStepDown: 10, force: true}));
    assert.commandWorked(primary.getDB("admin").runCommand({replSetFreeze: 30}));
    sleep(1000);

    // Node 1 will then step up to become a primary after the election timeout because node 0 is
    // frozen and node 2 has priority 0 so neither of them can start elections.
    replTest.waitForState(replTest.nodes[1], ReplSetTest.State.PRIMARY);

    // Unfreeze the primary before 30 seconds are up. The old primary will step up again now that it
    // can start elections again.
    assert.commandWorked(primary.getDB("admin").runCommand({replSetFreeze: 0}));
    replTest.waitForState(primary, ReplSetTest.State.PRIMARY);

    replTest.stopSet();
}

function dumpOplogEntries() {
    const replTest = new ReplSetTest({name: 'dumpOplogEntries', nodes: 3});
    replTest.startSet();

    // Call `initiate` without `initiateWithDefaultElectionTimeout: true` when not testing election
    // behavior. This initiates the replica set with high election timeout, which prevents unplanned
    // elections from happening during the test, so we can maintain the same primary throughout the
    // test.
    replTest.initiate();

    // Insert some documents.
    const primary = replTest.getPrimary();
    const primaryDB = primary.getDB("db");
    assert.commandWorked(primaryDB["collection"].insertMany(
        [...Array(5).keys()].map(x => ({a: x.toString()})), {ordered: false}));

    // Use `dumpOplog()` to print out oplog entries to help with debugging.
    replTest.dumpOplog(primary);

    // Shut down the set and finish the test.
    replTest.stopSet();
}

nodeWithHighestPriorityStepsUp();
manuallyStepUpNodeWhenHighElectionTimeoutSet();
frozenNodesDontTriggerElections();
dumpOplogEntries();
