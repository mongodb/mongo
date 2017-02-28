/**
 * Check that on a loss of primary, another node doesn't assume primary if it is stale. We force a
 * stepDown to test this.
 *
 * This test requires the fsync command to force a secondary to be stale.
 * @tags: [requires_fsync]
 */

load("jstests/replsets/rslib.js");

var replTest = new ReplSetTest({
    name: 'testSet',
    nodes: {"n0": {rsConfig: {priority: 2}}, "n1": {}, "n2": {rsConfig: {votes: 1, priority: 0}}},
    nodeOptions: {verbose: 1}
});
var nodes = replTest.startSet();
replTest.initiate();
replTest.waitForState(nodes[0], ReplSetTest.State.PRIMARY);
var master = replTest.getPrimary();

// do a write
print("\ndo a write");
assert.writeOK(master.getDB("foo").bar.insert({x: 1}));
replTest.awaitReplication();

// In the event of any error, we have to unlock any nodes that we have fsyncLocked.
function unlockNodes(nodes) {
    jsTestLog('Unlocking nodes: ' + tojson(nodes));
    nodes.forEach(function(node) {
        try {
            jsTestLog('Unlocking node: ' + node);
            assert.commandWorked(node.getDB("admin").fsyncUnlock());
        } catch (e) {
            jsTestLog('Failed to unlock node: ' + node + ': ' + tojson(e) +
                      '. Ignoring unlock error and moving on to next node.');
        }
    });
}

var lockedNodes = [];
try {
    // lock secondaries
    jsTestLog('Locking nodes: ' + tojson(replTest.liveNodes.slaves));
    replTest.liveNodes.slaves.forEach(function(node) {
        jsTestLog('Locking node: ' + node);
        jsTestLog(
            'fsync lock ' + node + ' result: ' +
            tojson(assert.commandWorked(node.getDB("admin").runCommand({fsync: 1, lock: 1}))));
        lockedNodes.push(node);
    });

    jsTestLog('Stepping down primary: ' + master);

    for (var i = 0; i < 11; i++) {
        // do another write
        assert.writeOK(master.getDB("foo").bar.insert({x: i}));
    }

    jsTestLog('Do stepdown of primary ' + master + ' that should not work');

    // this should fail, so we don't need to try/catch
    jsTestLog(
        'Step down ' + master + ' expected error: ' +
        tojson(assert.commandFailed(master.getDB("admin").runCommand({replSetStepDown: 10}))));

    // The server will disconnect the client on a successful forced stepdown so we use the
    // presence of an exception to confirm the forced stepdown result.
    jsTestLog('Do stepdown of primary ' + master + ' that should work');
    var exceptionFromForcedStepDown = assert.throws(function() {
        master.getDB("admin").runCommand(
            {replSetStepDown: ReplSetTest.kDefaultTimeoutMS, force: true});
    });
    jsTestLog('Forced stepdown ' + master + ' expected failure: ' +
              tojson(exceptionFromForcedStepDown));

    jsTestLog('Checking isMaster on ' + master);
    var r2 = assert.commandWorked(master.getDB("admin").runCommand({ismaster: 1}));
    jsTestLog('Result from running isMaster on ' + master + ': ' + tojson(r2));
    assert.eq(r2.ismaster, false);
    assert.eq(r2.secondary, true);
} catch (e) {
    throw e;
} finally {
    unlockNodes(lockedNodes);
}

print("\nreset stepped down time");
assert.commandWorked(master.getDB("admin").runCommand({replSetFreeze: 0}));
master = replTest.getPrimary();

print("\nawait");
replTest.awaitSecondaryNodes(90000);
replTest.awaitReplication();

// 'n0' may have just voted for 'n1', preventing it from becoming primary for the first 30 seconds
// of this assert.soon
assert.soon(function() {
    try {
        var result = master.getDB("admin").runCommand({isMaster: 1});
        return new RegExp(":" + replTest.nodes[0].port + "$").test(result.primary);
    } catch (x) {
        return false;
    }
}, "wait for n0 to be primary", 60000);

master = replTest.getPrimary();
var firstMaster = master;
print("\nmaster is now " + firstMaster);

try {
    assert.commandWorked(master.getDB("admin").runCommand({replSetStepDown: 100, force: true}));
} catch (e) {
    // ignore errors due to connection failures as we expect the master to close connections
    // on stepdown
    if (!isNetworkError(e)) {
        throw e;
    }
}

print("\nget a master");
replTest.getPrimary();

assert.soon(function() {
    var secondMaster = replTest.getPrimary();
    return firstMaster.host !== secondMaster.host;
}, "making sure " + firstMaster.host + " isn't still master", 60000);

// Add arbiter for shutdown tests
replTest.add();
print("\ncheck shutdown command");

master = replTest.liveNodes.master;
var slave = replTest.liveNodes.slaves[0];

try {
    slave.adminCommand({shutdown: 1});
} catch (e) {
    print(e);
}

master = replTest.getPrimary();
assert.soon(function() {
    try {
        var result = master.getDB("admin").runCommand({replSetGetStatus: 1});
        for (var i in result.members) {
            if (result.members[i].self) {
                continue;
            }

            return result.members[i].health == 0;
        }
    } catch (e) {
        print("error getting status from master: " + e);
        master = replTest.getPrimary();
        return false;
    }
}, 'make sure master knows that slave is down before proceeding');

print("\nrunning shutdown without force on master: " + master);

// this should fail because the master can't reach an up-to-date secondary (because the only
// secondary is down)
var now = new Date();
assert.commandFailed(master.getDB("admin").runCommand({shutdown: 1, timeoutSecs: 3}));
// on windows, javascript and the server perceive time differently, to compensate here we use 2750ms
assert.gte((new Date()) - now, 2750);

print("\nsend shutdown command");

var currentMaster = replTest.getPrimary();
try {
    printjson(currentMaster.getDB("admin").runCommand({shutdown: 1, force: true}));
} catch (e) {
    if (!isNetworkError(e)) {
        throw e;
    }
}

print("checking " + currentMaster + " is actually shutting down");
assert.soon(function() {
    try {
        currentMaster.findOne();
    } catch (e) {
        return true;
    }
    return false;
});

print("\nOK 1");

replTest.stopSet();

print("OK 2");
