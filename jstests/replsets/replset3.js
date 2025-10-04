import {ReplSetTest} from "jstests/libs/replsettest.js";

let doTest = function (signal) {
    // Test replica set step down

    // Replica set testing API
    // Create a new replica set test. Specify set name and the number of nodes you want.
    let replTest = new ReplSetTest({name: "testSet", nodes: 3});

    // call startSet() to start each mongod in the replica set
    // this returns a list of nodes
    let nodes = replTest.startSet();

    // Call initiate() to send the replSetInitiate command
    // This will wait for initiation
    replTest.initiate(null, null, {initiateWithDefaultElectionTimeout: true});

    // Get primary node
    let primary = replTest.getPrimary();

    // Write some data to primary
    // NOTE: this test fails unless we write some data.
    primary.getDB("foo").foo.insert({a: 1}, {writeConcern: {w: 3, wtimeout: 20000}});

    let phase = 1;

    print(phase++);

    // Step down primary.
    assert.commandWorked(primary.getDB("admin").runCommand({replSetStepDown: 0, force: 1}));

    print(phase++);

    try {
        var newPrimary = replTest.getPrimary();
    } catch (err) {
        throw "Could not elect new primary before timeout.";
    }

    print(phase++);

    assert(primary != newPrimary, "Old primary shouldn't be equal to new primary.");

    print(phase++);

    // Make sure that secondaries are still up
    let result = newPrimary.getDB("admin").runCommand({replSetGetStatus: 1});
    assert(result["ok"] == 1, "Could not verify that secondaries were still up:" + result);

    print(phase++);

    let secondaries = replTest.getSecondaries();
    assert.soon(function () {
        try {
            var res = secondaries[0].getDB("admin").runCommand({replSetGetStatus: 1});
        } catch (err) {}
        return res.myState == 2;
    }, "Secondary 0 state not ready.");

    print(phase++);

    assert.soon(function () {
        try {
            var res = secondaries[1].getDB("admin").runCommand({replSetGetStatus: 1});
        } catch (err) {}
        return res.myState == 2;
    }, "Secondary 1 state not ready.");

    print("replset3.js SUCCESS");

    replTest.stopSet(15);
};

doTest(15);
