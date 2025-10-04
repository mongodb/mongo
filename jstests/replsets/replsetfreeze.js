/*
 * 1: initialize set
 * 2: step down m1
 * 3: freeze set for 30 seconds
 * 4: check no one is primary for 30 seconds
 * 5: check for new primary
 * 6: step down new primary
 * 7: freeze for 30 seconds
 * 8: unfreeze
 * 9: check we get a new primary within 30 seconds
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

let w = 0;
let wait = function (f) {
    w++;
    let n = 0;
    while (!f()) {
        if (n % 4 == 0) print("toostale.js waiting " + w);
        if (++n == 4) {
            print("" + f);
        }
        assert(n < 200, "tried 200 times, giving up");
        sleep(1000);
    }
};

let reconnect = function (a) {
    wait(function () {
        try {
            a.getDB("foo").bar.stats();
            return true;
        } catch (e) {
            print(e);
            return false;
        }
    });
};

jsTestLog("1: initialize set");
let replTest = new ReplSetTest({name: "unicomplex", nodes: 3});
let nodes = replTest.nodeList();
let conns = replTest.startSet();
let config = {
    "_id": "unicomplex",
    "members": [
        {"_id": 0, "host": nodes[0]},
        {"_id": 1, "host": nodes[1]},
        {"_id": 2, "host": nodes[2], "arbiterOnly": true},
    ],
};
let r = replTest.initiate(config, null, {initiateWithDefaultElectionTimeout: true});

replTest.awaitNodesAgreeOnPrimary();

let primary = replTest.getPrimary();

let secondary = replTest.getSecondary();
jsTestLog("2: freeze secondary " + secondary.host + " so that it does not run for election for the rest of the test");

assert.commandWorked(secondary.getDB("admin").runCommand({replSetFreeze: 600}));

assert.commandFailedWithCode(
    primary.getDB("admin").runCommand({replSetFreeze: 30}),
    ErrorCodes.NotSecondary,
    "replSetFreeze should return error when run on primary " + primary.host,
);

jsTestLog("3: step down primary " + primary.host);
assert.commandWorked(primary.getDB("admin").runCommand({replSetStepDown: 10, force: 1}));
printjson(primary.getDB("admin").runCommand({replSetGetStatus: 1}));

jsTestLog("4: freeze stepped down primary " + primary.host + " for 30 seconds");
let start = new Date().getTime();
assert.commandWorked(primary.getDB("admin").runCommand({replSetFreeze: 30}));

jsTestLog("5: check no one is primary for 30 seconds");
while (new Date().getTime() - start < 28 * 1000) {
    // we need less 30 since it takes some time to return... hacky
    let result = primary.getDB("admin").runCommand({hello: 1});
    assert.eq(result.isWritablePrimary, false);
    assert.eq(result.primary, undefined);
    sleep(1000);
}

jsTestLog("6: check for new primary");
let newPrimary = replTest.getPrimary();
assert.eq(primary.host, newPrimary.host, "new primary should be the same node as primary that previously stepped down");

jsTestLog("7: step down new primary " + primary.host);
assert.commandWorked(primary.getDB("admin").runCommand({replSetStepDown: 10, force: 1}));

jsTestLog("8: freeze stepped down primary " + primary.host + " for 30 seconds");
primary.getDB("admin").runCommand({replSetFreeze: 30});
sleep(1000);

jsTestLog("9: unfreeze stepped down primary " + primary.host + " after waiting for 1 second");
primary.getDB("admin").runCommand({replSetFreeze: 0});

jsTestLog("10: wait for unfrozen node " + primary.host + " to become primary again");
newPrimary = replTest.getPrimary();
jsTestLog("Primary after unfreezing node: " + newPrimary.host);
assert.eq(
    primary.host,
    newPrimary.host,
    "new primary after unfreezing should be the same node as primary that previously stepped down",
);

replTest.stopSet(15);
