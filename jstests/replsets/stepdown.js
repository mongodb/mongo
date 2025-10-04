/**
 * Check that on a loss of primary, another node doesn't assume primary if it is stale. We force a
 * stepDown to test this.
 *
 * This test also checks that the serverStatus command metrics replSetStepDown and
 * replSetStepDownWithForce are incremented correctly.
 *
 * This test requires the fsync command to force a secondary to be stale.
 * @tags: [requires_fsync]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {verifyServerStatusChange} from "jstests/replsets/libs/election_metrics.js";

// We are bypassing collection validation because this test runs "shutdown" command so the server is
// expected to be down when MongoRunner.stopMongod is called.
TestData.skipCollectionAndIndexValidation = true;

let replTest = new ReplSetTest({
    name: "testSet",
    nodes: {"n0": {rsConfig: {priority: 2}}, "n1": {}, "n2": {rsConfig: {votes: 1, priority: 0}}},
    nodeOptions: {verbose: 1},
});
let nodes = replTest.startSet();
replTest.initiate(null, null, {initiateWithDefaultElectionTimeout: true});
replTest.waitForState(nodes[0], ReplSetTest.State.PRIMARY);
let primary = replTest.getPrimary();

// The default WC is majority and this test can't satisfy majority writes.
assert.commandWorked(
    primary.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}),
);

// do a write
print("\ndo a write");
assert.commandWorked(primary.getDB("foo").bar.insert({x: 1}));
replTest.awaitReplication();

// In the event of any error, we have to unlock any nodes that we have fsyncLocked.
function unlockNodes(nodes) {
    jsTestLog("Unlocking nodes: " + tojson(nodes));
    nodes.forEach(function (node) {
        try {
            jsTestLog("Unlocking node: " + node);
            assert.commandWorked(node.getDB("admin").fsyncUnlock());
        } catch (e) {
            jsTestLog(
                "Failed to unlock node: " +
                    node +
                    ": " +
                    tojson(e) +
                    ". Ignoring unlock error and moving on to next node.",
            );
        }
    });
}

let lockedNodes = [];
try {
    // lock secondaries
    jsTestLog("Locking nodes: " + tojson(replTest.getSecondaries()));
    replTest.getSecondaries().forEach(function (node) {
        jsTestLog("Locking node: " + node);
        jsTestLog(
            "fsync lock " +
                node +
                " result: " +
                tojson(assert.commandWorked(node.getDB("admin").runCommand({fsync: 1, lock: 1}))),
        );
        lockedNodes.push(node);
    });

    jsTestLog("Stepping down primary: " + primary);

    for (let i = 0; i < 11; i++) {
        // do another write
        assert.commandWorked(primary.getDB("foo").bar.insert({x: i}));
    }

    let res = assert.commandWorked(primary.adminCommand({replSetGetStatus: 1}));
    assert(
        res.electionCandidateMetrics,
        () => "Response should have an 'electionCandidateMetrics' field: " + tojson(res),
    );
    let intitialServerStatus = assert.commandWorked(primary.adminCommand({serverStatus: 1}));

    jsTestLog("Do stepdown of primary " + primary + " that should not work");

    // this should fail, so we don't need to try/catch
    jsTestLog(
        "Step down " +
            primary +
            " expected error: " +
            tojson(assert.commandFailed(primary.getDB("admin").runCommand({replSetStepDown: 10}))),
    );

    // Check that the 'total' and 'failed' fields of 'replSetStepDown' have been incremented in
    // serverStatus and that they have not been incremented for 'replSetStepDownWithForce'.
    let newServerStatus = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
    verifyServerStatusChange(
        intitialServerStatus.metrics.commands.replSetStepDown,
        newServerStatus.metrics.commands.replSetStepDown,
        "total",
        1,
    );
    verifyServerStatusChange(
        intitialServerStatus.metrics.commands.replSetStepDown,
        newServerStatus.metrics.commands.replSetStepDown,
        "failed",
        1,
    );
    verifyServerStatusChange(
        intitialServerStatus.metrics.commands.replSetStepDownWithForce,
        newServerStatus.metrics.commands.replSetStepDownWithForce,
        "total",
        0,
    );
    verifyServerStatusChange(
        intitialServerStatus.metrics.commands.replSetStepDownWithForce,
        newServerStatus.metrics.commands.replSetStepDownWithForce,
        "failed",
        0,
    );

    // This section checks that the metrics are incremented accurately when the command fails due to
    // an error occurring before stepDown is called in the replication coordinator, such as due to
    // bad values or type mismatches in the arguments, or checkReplEnabledForCommand returning a bad
    // status. The stepdown period being negative is one example of such an error, but success in
    // this case gives us confidence that the behavior in the other cases is the same.

    // Stepdown should fail because the stepdown period is negative
    jsTestLog("Do stepdown of primary " + primary + " that should not work");
    assert.commandFailedWithCode(
        primary.getDB("admin").runCommand({replSetStepDown: -1, force: true}),
        ErrorCodes.BadValue,
    );

    // Check that the 'total' and 'failed' fields of 'replSetStepDown' and
    // 'replSetStepDownWithForce' have been incremented in serverStatus.
    intitialServerStatus = newServerStatus;
    newServerStatus = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
    verifyServerStatusChange(
        intitialServerStatus.metrics.commands.replSetStepDown,
        newServerStatus.metrics.commands.replSetStepDown,
        "total",
        1,
    );
    verifyServerStatusChange(
        intitialServerStatus.metrics.commands.replSetStepDown,
        newServerStatus.metrics.commands.replSetStepDown,
        "failed",
        1,
    );
    verifyServerStatusChange(
        intitialServerStatus.metrics.commands.replSetStepDownWithForce,
        newServerStatus.metrics.commands.replSetStepDownWithForce,
        "total",
        1,
    );
    verifyServerStatusChange(
        intitialServerStatus.metrics.commands.replSetStepDownWithForce,
        newServerStatus.metrics.commands.replSetStepDownWithForce,
        "failed",
        1,
    );

    jsTestLog("Do stepdown of primary " + primary + " that should work");
    assert.commandWorked(primary.adminCommand({replSetStepDown: ReplSetTest.kDefaultTimeoutMS, force: true}));

    // Check that the 'total' fields of 'replSetStepDown' and 'replSetStepDownWithForce' have been
    // incremented in serverStatus and that their 'failed' fields have not been incremented.
    intitialServerStatus = newServerStatus;
    newServerStatus = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
    verifyServerStatusChange(
        intitialServerStatus.metrics.commands.replSetStepDown,
        newServerStatus.metrics.commands.replSetStepDown,
        "total",
        1,
    );
    verifyServerStatusChange(
        intitialServerStatus.metrics.commands.replSetStepDown,
        newServerStatus.metrics.commands.replSetStepDown,
        "failed",
        0,
    );
    verifyServerStatusChange(
        intitialServerStatus.metrics.commands.replSetStepDownWithForce,
        newServerStatus.metrics.commands.replSetStepDownWithForce,
        "total",
        1,
    );
    verifyServerStatusChange(
        intitialServerStatus.metrics.commands.replSetStepDownWithForce,
        newServerStatus.metrics.commands.replSetStepDownWithForce,
        "failed",
        0,
    );

    jsTestLog("Checking hello on " + primary);
    let r2 = assert.commandWorked(primary.getDB("admin").runCommand({hello: 1}));
    jsTestLog("Result from running hello on " + primary + ": " + tojson(r2));
    assert.eq(r2.isWritablePrimary, false);
    assert.eq(r2.secondary, true);

    // Check that the 'electionCandidateMetrics' section of the replSetGetStatus response has been
    // cleared, since the node is no longer primary.
    res = assert.commandWorked(primary.adminCommand({replSetGetStatus: 1}));
    assert(
        !res.electionCandidateMetrics,
        () => "Response should not have an 'electionCandidateMetrics' field: " + tojson(res),
    );

    // This section checks that the metrics are incremented accurately when the command fails due to
    // an error while stepping down. This is one reason the replSetStepDown command could fail once
    // we call stepDown in the replication coordinator, but success in this case gives us confidence
    // that the behavior in the other cases is the same.

    // Stepdown should fail because the node is no longer primary
    jsTestLog("Do stepdown of primary " + primary + " that should not work");
    assert.commandFailedWithCode(
        primary.getDB("admin").runCommand({replSetStepDown: ReplSetTest.kDefaultTimeoutMS, force: true}),
        ErrorCodes.NotWritablePrimary,
    );

    // Check that the 'total' and 'failed' fields of 'replSetStepDown' and
    // 'replSetStepDownWithForce' have been incremented in serverStatus.
    intitialServerStatus = newServerStatus;
    newServerStatus = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
    verifyServerStatusChange(
        intitialServerStatus.metrics.commands.replSetStepDown,
        newServerStatus.metrics.commands.replSetStepDown,
        "total",
        1,
    );
    verifyServerStatusChange(
        intitialServerStatus.metrics.commands.replSetStepDown,
        newServerStatus.metrics.commands.replSetStepDown,
        "failed",
        1,
    );
    verifyServerStatusChange(
        intitialServerStatus.metrics.commands.replSetStepDownWithForce,
        newServerStatus.metrics.commands.replSetStepDownWithForce,
        "total",
        1,
    );
    verifyServerStatusChange(
        intitialServerStatus.metrics.commands.replSetStepDownWithForce,
        newServerStatus.metrics.commands.replSetStepDownWithForce,
        "failed",
        1,
    );
} finally {
    unlockNodes(lockedNodes);
}

print("\nreset stepped down time");
assert.commandWorked(primary.getDB("admin").runCommand({replSetFreeze: 0}));
primary = replTest.getPrimary();

print("\nawait");
replTest.awaitSecondaryNodes(90000);
replTest.awaitReplication();

// 'n0' may have just voted for 'n1', preventing it from becoming primary for the first 30 seconds
// of this assert.soon
assert.soon(
    function () {
        try {
            let result = primary.getDB("admin").runCommand({hello: 1});
            return new RegExp(":" + replTest.nodes[0].port + "$").test(result.primary);
        } catch (x) {
            return false;
        }
    },
    "wait for n0 to be primary",
    60000,
);

primary = replTest.getPrimary();
let firstPrimary = primary;
print("\nprimary is now " + firstPrimary);

assert.adminCommandWorkedAllowingNetworkError(primary, {replSetStepDown: 100, force: true});

print("\nget a primary");
replTest.getPrimary();

assert.soon(
    function () {
        let secondPrimary = replTest.getPrimary();
        return firstPrimary.host !== secondPrimary.host;
    },
    "making sure " + firstPrimary.host + " isn't still primary",
    60000,
);

// Add arbiter for shutdown tests
replTest.add();
print("\ncheck shutdown command");

primary = replTest.getPrimary();
let secondary = replTest.getSecondary();

try {
    secondary.adminCommand({shutdown: 1});
} catch (e) {
    print(e);
}

primary = replTest.getPrimary();
assert.soon(function () {
    try {
        let result = primary.getDB("admin").runCommand({replSetGetStatus: 1});
        for (let i in result.members) {
            if (result.members[i].self) {
                continue;
            }

            return result.members[i].health == 0;
        }
    } catch (e) {
        print("error getting status from primary: " + e);
        primary = replTest.getPrimary();
        return false;
    }
}, "make sure primary knows that secondary is down before proceeding");

print("\nrunning shutdown without force on primary: " + primary);

// this should fail because the primary can't reach an up-to-date secondary (because the only
// secondary is down)
let now = new Date();
assert.commandFailed(primary.getDB("admin").runCommand({shutdown: 1, timeoutSecs: 3}));
// on windows, javascript and the server perceive time differently, to compensate here we use 2750ms
assert.gte(new Date() - now, 2750);

print("\nsend shutdown command");

let currentPrimary = replTest.getPrimary();
try {
    printjson(currentPrimary.getDB("admin").runCommand({shutdown: 1, force: true}));
} catch (e) {
    if (!isNetworkError(e)) {
        throw e;
    }
}

print("checking " + currentPrimary + " is actually shutting down");
assert.soon(function () {
    try {
        currentPrimary.findOne();
    } catch (e) {
        return true;
    }
    return false;
});

print("\nOK 1");

replTest.stopSet();

print("OK 2");
