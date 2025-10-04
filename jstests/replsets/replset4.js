import {ReplSetTest} from "jstests/libs/replsettest.js";

let doTest = function (signal) {
    // Test orphaned primary steps down
    let replTest = new ReplSetTest({name: "testSet", nodes: 3});

    replTest.startSet();
    replTest.initiate(null, null, {initiateWithDefaultElectionTimeout: true});

    let primary = replTest.getPrimary();

    // Kill both secondaries, simulating a network partition
    let secondaries = replTest.getSecondaries();
    for (let i = 0; i < secondaries.length; i++) {
        let secondary_id = replTest.getNodeId(secondaries[i]);
        replTest.stop(secondary_id);
    }

    print("replset4.js 1");

    assert.soon(function () {
        try {
            let result = primary.getDB("admin").runCommand({hello: 1});
            return result["ok"] == 1 && result["isWritablePrimary"] == false;
        } catch (e) {
            print("replset4.js caught " + e);
            return false;
        }
    }, "Primary fails to step down when orphaned.");

    print("replset4.js worked, stopping");
    replTest.stopSet(signal);
};

print("replset4.js");
doTest(15);
print("replset4.js SUCCESS");
