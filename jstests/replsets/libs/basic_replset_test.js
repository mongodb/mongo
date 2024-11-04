import {ReplSetTest} from "jstests/libs/replsettest.js";
import {
    verifyServerStatusElectionReasonCounterValue
} from "jstests/replsets/libs/election_metrics.js";
import {reconnect} from "jstests/replsets/rslib.js";

export function basicReplsetTest(signal, ssl_options1, ssl_options2, ssl_name) {
    // Test basic replica set functionality.
    // -- Replication
    // -- Failover

    // Choose a name that is unique to the options specified.
    // This is important because we are depending on a fresh replicaSetMonitor for each run;
    // each differently-named replica set gets its own monitor.
    // n0 and n1 get the same SSL config since there are 3 nodes but only 2 different configs
    let replTest = new ReplSetTest({
        name: 'testSet' + ssl_name,
        nodes: {n0: ssl_options1, n1: ssl_options1, n2: ssl_options2}
    });

    // call startSet() to start each mongod in the replica set
    replTest.startSet();

    // Call initiate() to send the replSetInitiate command
    // This will wait for initiation
    replTest.initiate(null, null, {initiateWithDefaultElectionTimeout: true});

    // Call getPrimary to return a reference to the node that's been
    // elected primary.
    let primary = replTest.getPrimary();

    // Check that both the 'called' and 'successful' fields of the 'electionTimeout' election reason
    // counter have been incremented in serverStatus.
    const primaryStatus = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
    verifyServerStatusElectionReasonCounterValue(
        primaryStatus.electionMetrics, "electionTimeout", 1);

    // Ensure the primary logs an n-op to the oplog upon transitioning to primary.
    assert.gt(primary.getDB("local").oplog.rs.count({op: 'n', o: {msg: 'new primary'}}), 0);

    // Here's how you save something to primary
    primary.getDB("foo").foo.save({a: 1000});

    // This method will check the oplogs of the primary
    // and secondaries in the set and wait until the change has replicated.
    replTest.awaitReplication();

    let cppconn = new Mongo(replTest.getURL()).getDB("foo");
    assert.eq(1000, cppconn.foo.findOne().a, "cppconn 1");

    {
        // check c++ finding other servers
        let temp = replTest.getURL();
        temp = temp.substring(0, temp.lastIndexOf(","));
        temp = new Mongo(temp).getDB("foo");
        assert.eq(1000, temp.foo.findOne().a, "cppconn 1");
    }

    // Here's how to stop the primary node
    let primaryId = replTest.getNodeId(primary);
    replTest.stop(primaryId);

    // Now let's see who the new primary is:
    let newPrimary = replTest.getPrimary();

    // Is the new primary the same as the old primary?
    let newPrimaryId = replTest.getNodeId(newPrimary);

    assert(primaryId != newPrimaryId, "Old primary shouldn't be equal to new primary.");

    reconnect(cppconn);
    assert.eq(1000, cppconn.foo.findOne().a, "cppconn 2");

    // Now let's write some documents to the new primary
    let bulk = newPrimary.getDB("bar").bar.initializeUnorderedBulkOp();
    for (let i = 0; i < 1000; i++) {
        bulk.insert({a: i});
    }
    bulk.execute();

    // Here's how to restart the old primary node:
    let secondary = replTest.restart(primaryId);

    // Now, let's make sure that the old primary comes up as a secondary
    assert.soon(function() {
        let res = secondary.getDB("admin").runCommand({hello: 1});
        printjson(res);
        return res['ok'] == 1 && res['isWritablePrimary'] == false;
    });

    // And we need to make sure that the replset comes back up
    assert.soon(function() {
        let res = newPrimary.getDB("admin").runCommand({replSetGetStatus: 1});
        printjson(res);
        return res.myState == 1;
    });

    // And that both secondary nodes have all the updates
    newPrimary = replTest.getPrimary();
    assert.eq(1000, newPrimary.getDB("bar").runCommand({count: "bar"}).n, "assumption 2");
    replTest.awaitSecondaryNodes();
    replTest.awaitReplication();

    let secondaries = replTest.getSecondaries();
    assert(secondaries.length == 2, "Expected 2 secondaries but length was " + secondaries.length);
    secondaries.forEach(function(secondary) {
        secondary.setSecondaryOk();
        let count = secondary.getDB("bar").runCommand({count: "bar"});
        printjson(count);
        assert.eq(1000, count.n, "secondary count wrong: " + secondary);
    });

    // last error
    primary = replTest.getPrimary();
    secondaries = replTest.getSecondaries();

    let db = primary.getDB("foo");
    let t = db.foo;

    let ts = secondaries.map(function(z) {
        z.setSecondaryOk();
        return z.getDB("foo").foo;
    });

    t.save({a: 1000});
    t.createIndex({a: 1});
    replTest.awaitReplication();

    ts.forEach(function(z) {
        assert.eq(2, z.getIndexKeys().length, "A " + z.getMongo());
    });

    // Shut down the set and finish the test.
    replTest.stopSet(signal);
}

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
