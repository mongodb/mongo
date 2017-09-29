// Test stepdown during drain mode
// 1. Set up a 3-node set. Assume Node 0 is the primary at the beginning for simplicity.
// 2. Prevent applying retrieved ops on all secondaries, including Node 1.
// 3. Insert data to ensure Node 1 has ops to apply in its queue.
// 4. Step up Node 1. Now it enters drain mode, but cannot proceed.
// 5. Block Node 1's ability to process stepdowns.
// 5. Shut down nodes 0 and 2. Wait until Node 1 begins stepping down due to no longer having a
//    majority
// 6. Re-enable Node 1's ability to apply operations, ensure that clearing it's buffer doesn't
//    cause it to finish drain mode because of the pending stepdown request.
// 7. Allow Node 1 to finish stepping down.

(function() {
    "use strict";

    load("jstests/replsets/rslib.js");
    load("jstests/libs/check_log.js");

    var replSet = new ReplSetTest({name: 'testSet', nodes: 3});
    var nodes = replSet.nodeList();
    replSet.startSet();
    var conf = replSet.getReplSetConfig();
    conf.members[2].priority = 0;
    conf.settings = conf.settings || {};
    conf.settings.chainingAllowed = false;
    conf.settings.catchUpTimeoutMillis = 0;
    conf.protocolVersion = 1;
    replSet.initiate(conf);

    var primary = replSet.getPrimary();
    var secondary = replSet.getSecondary();

    // Set verbosity for replication on all nodes.
    var verbosity = {
        "setParameter": 1,
        "logComponentVerbosity": {
            "replication": {"verbosity": 3},
        }
    };
    replSet.nodes.forEach(function(node) {
        node.adminCommand(verbosity);
    });

    function enableFailPoint(node) {
        jsTest.log("enable failpoint " + node.host);
        assert.commandWorked(
            node.adminCommand({configureFailPoint: 'rsSyncApplyStop', mode: 'alwaysOn'}));
    }

    function disableFailPoint(node) {
        jsTest.log("disable failpoint " + node.host);
        assert.commandWorked(
            node.adminCommand({configureFailPoint: 'rsSyncApplyStop', mode: 'off'}));
    }

    // Since this test blocks a node in drain mode, we cannot use the ReplSetTest stepUp helper
    // that waits for a node to leave drain mode.
    function stepUpNode(node) {
        jsTest.log("Stepping up: " + node.host);
        assert.soonNoExcept(function() {
            assert.commandWorked(node.adminCommand({replSetStepUp: 1}));
            // We do not specify a specific primary so that if a different primary gets elected
            // due to unfortunate timing we can try again.
            replSet.awaitNodesAgreeOnPrimary();
            return node.adminCommand('replSetGetStatus').myState === ReplSetTest.State.PRIMARY;
        }, 'failed to step up node ' + node.host, replSet.kDefaultTimeoutMS);
    }

    // Do an initial insert to prevent the secondary from going into recovery
    var numDocuments = 20;
    var coll = primary.getDB("foo").foo;
    assert.writeOK(coll.insert({x: 0}, {writeConcern: {w: 3}}));
    replSet.awaitReplication();

    // Enable fail point to stop replication.
    var secondaries = replSet.getSecondaries();
    secondaries.forEach(enableFailPoint);

    var bufferCountBefore = secondary.getDB('foo').serverStatus().metrics.repl.buffer.count;
    for (var i = 1; i < numDocuments; ++i) {
        assert.writeOK(coll.insert({x: i}));
    }
    jsTestLog('Number of documents inserted into collection on primary: ' + numDocuments);
    assert.eq(numDocuments, primary.getDB("foo").foo.find().itcount());

    assert.soon(
        function() {
            var serverStatus = secondary.getDB('foo').serverStatus();
            var bufferCount = serverStatus.metrics.repl.buffer.count;
            var bufferCountChange = bufferCount - bufferCountBefore;
            jsTestLog('Number of operations buffered on secondary since stopping applier: ' +
                      bufferCountChange);
            return bufferCountChange == numDocuments - 1;
        },
        'secondary did not buffer operations for new inserts on primary',
        replSet.kDefaultTimeoutMs,
        1000);

    reconnect(secondary);
    stepUpNode(secondary);

    // Secondary doesn't allow writes yet.
    var res = secondary.getDB("admin").runCommand({"isMaster": 1});
    assert(!res.ismaster);

    assert.commandFailedWithCode(
        secondary.adminCommand({
            replSetTest: 1,
            waitForDrainFinish: 5000,
        }),
        ErrorCodes.ExceededTimeLimit,
        'replSetTest waitForDrainFinish should time out when draining is not allowed to complete');

    // Prevent the current primary from stepping down
    jsTest.log("disallowing heartbeat stepdown " + secondary.host);
    assert.commandWorked(
        secondary.adminCommand({configureFailPoint: "blockHeartbeatStepdown", mode: 'alwaysOn'}));
    jsTestLog("Shut down the rest of the set so the primary-elect has to step down");
    replSet.stop(primary);
    disableFailPoint(replSet.nodes[2]);  // Fail point needs to be off when node is shut down.
    replSet.stop(2);

    jsTestLog("Waiting for secondary to begin stepping down while in drain mode");
    checkLog.contains(secondary, "stepDown - blockHeartbeatStepdown fail point enabled");

    // Disable fail point to allow replication and allow secondary to finish drain mode while in the
    // process of stepping down.
    jsTestLog("Re-enabling replication on secondary");
    assert.gt(numDocuments, secondary.getDB("foo").foo.find().itcount());
    disableFailPoint(secondary);

    // The node should now be able to apply the writes in its buffer.
    jsTestLog("Waiting for node to drain its apply buffer");
    assert.soon(function() {
        return secondary.getDB("foo").foo.find().itcount() == numDocuments;
    });

    // Even though it finished draining its buffer, it shouldn't be able to exit drain mode due to
    // pending stepdown.
    assert.commandFailedWithCode(
        secondary.adminCommand({
            replSetTest: 1,
            waitForDrainFinish: 5000,
        }),
        ErrorCodes.ExceededTimeLimit,
        'replSetTest waitForDrainFinish should time out when in the middle of stepping down');

    jsTestLog("Checking that node is PRIMARY but not master");
    assert.eq(ReplSetTest.State.PRIMARY, secondary.adminCommand({replSetGetStatus: 1}).myState);
    assert(!secondary.adminCommand('ismaster').ismaster);

    jsTest.log("allowing heartbeat stepdown " + secondary.host);
    assert.commandWorked(
        secondary.adminCommand({configureFailPoint: "blockHeartbeatStepdown", mode: 'off'}));

    jsTestLog("Checking that node successfully stepped down");
    replSet.waitForState(secondary, ReplSetTest.State.SECONDARY);
    assert(!secondary.adminCommand('ismaster').ismaster);

    // Now ensure that the node can successfully become primary again.
    replSet.restart(0);
    replSet.restart(2);
    stepUpNode(secondary);

    assert.soon(function() {
        return secondary.adminCommand('ismaster').ismaster;
    });

    jsTestLog('Ensure new primary is writable.');
    assert.writeOK(secondary.getDB("foo").flag.insert({sentinel: 1}, {writeConcern: {w: 3}}));
    // Check that no writes were lost.
    assert.eq(secondary.getDB("foo").foo.find().itcount(), numDocuments);
})();
