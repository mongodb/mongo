// Test stepdown dring drain mode
// 1. Set up a 3-node set. Assume Node 1 is the primary at the beginning for simplicity.
// 2. Prevent applying retrieved ops on all secondaries, including Node 2.
// 3. Insert data to ensure Node 2 has ops to apply in its queue.
// 4. Step up Node 2. Now it enters drain mode, but cannot proceed.
// 5. Step up Node 1. Wait until Node 2 knows of a higher term and steps down.
//    Node 2 re-enables bgsync producer while it's still in drain mode.
// 6. Step up Node 2 again. It enters drain mode again.
// 7. Enable applying ops.
// 8. Ensure the ops in queue are applied and that Node 2 begins to accept writes as usual.

load("jstests/replsets/rslib.js");

(function() {
    "use strict";
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

    function stepUpNode(node) {
        assert.soonNoExcept(function() {
            assert.commandWorked(node.adminCommand({replSetStepUp: 1}));
            replSet.awaitNodesAgreeOnPrimary(
                replSet.kDefaultTimeoutMS, replSet.nodes, replSet.getNodeId(node));
            return node.adminCommand('replSetGetStatus').myState == ReplSetTest.State.PRIMARY;
        }, 'failed to step up node ' + node.host, replSet.kDefaultTimeoutMS);
    }

    // Do an initial insert to prevent the secondary from going into recovery
    var numDocuments = 20;
    var coll = primary.getDB("foo").foo;
    assert.writeOK(coll.insert({x: 0}, {writeConcern: {w: 3}}));
    replSet.awaitReplication(ReplSetTest.kDefaultTimeoutMS, ReplSetTest.OpTimeType.LAST_DURABLE);

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

    // Original primary steps up.
    reconnect(primary);
    stepUpNode(primary);

    reconnect(secondary);
    stepUpNode(secondary);

    // Disable fail point to allow replication.
    secondaries.forEach(disableFailPoint);

    assert.commandWorked(
        secondary.adminCommand({
            replSetTest: 1,
            waitForDrainFinish: 5000,
        }),
        'replSetTest waitForDrainFinish should work when draining is allowed to complete');

    // Ensure new primary is writable.
    jsTestLog('New primary should be writable after draining is complete');
    assert.writeOK(secondary.getDB("foo").flag.insert({sentinel: 1}));
    // Check that all writes reached the secondary's op queue prior to
    // stepping down the original primary and got applied.
    assert.eq(secondary.getDB("foo").foo.find().itcount(), numDocuments);
})();
