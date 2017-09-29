// Test that the stepdown command can be run successfully during drain mode

(function() {
    "use strict";

    load("jstests/replsets/rslib.js");

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

    try {
        secondary.adminCommand({replSetStepDown: 60, force: true});
    } catch (e) {
        // expected
        print("Caught stepdown exception: " + tojson(e));
    }

    // Assert stepdown was successful.
    reconnect(secondary);
    assert.eq(ReplSetTest.State.SECONDARY, secondary.adminCommand({replSetGetStatus: 1}).myState);
    assert(!secondary.adminCommand('ismaster').ismaster);

    // Prevent the producer from fetching new ops
    assert.commandWorked(
        secondary.adminCommand({configureFailPoint: 'stopReplProducer', mode: 'alwaysOn'}));

    // Allow the secondary to apply the ops already in its buffer.
    jsTestLog("Re-enabling replication on secondaries");
    assert.gt(numDocuments, secondary.getDB("foo").foo.find().itcount());
    secondaries.forEach(disableFailPoint);

    // The node should now be able to apply the writes in its buffer.
    jsTestLog("Waiting for node to drain its apply buffer");
    assert.soon(function() {
        return secondary.getDB("foo").foo.find().itcount() == numDocuments;
    });
})();
