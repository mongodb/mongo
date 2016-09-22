/*
 * 1: initialize set
 * 2: step down m1
 * 3: freeze set for 30 seconds
 * 4: check no one is master for 30 seconds
 * 5: check for new master
 * 6: step down new master
 * 7: freeze for 30 seconds
 * 8: unfreeze
 * 9: check we get a new master within 30 seconds
 */

var w = 0;
var wait = function(f) {
    w++;
    var n = 0;
    while (!f()) {
        if (n % 4 == 0)
            print("toostale.js waiting " + w);
        if (++n == 4) {
            print("" + f);
        }
        assert(n < 200, 'tried 200 times, giving up');
        sleep(1000);
    }
};

var reconnect = function(a) {
    wait(function() {
        try {
            a.getDB("foo").bar.stats();
            return true;
        } catch (e) {
            print(e);
            return false;
        }
    });
};

jsTestLog('1: initialize set');
var replTest = new ReplSetTest({name: 'unicomplex', nodes: 3});
var nodes = replTest.nodeList();
var conns = replTest.startSet();
var config = {
    "_id": "unicomplex",
    "members": [
        {"_id": 0, "host": nodes[0]},
        {"_id": 1, "host": nodes[1]},
        {"_id": 2, "host": nodes[2], "arbiterOnly": true}
    ]
};
var r = replTest.initiate(config);

replTest.awaitNodesAgreeOnPrimary();

var master = replTest.getPrimary();

var secondary = replTest.getSecondary();
jsTestLog('2: freeze secondary ' + secondary.host +
          ' so that it does not run for election for the rest of the test');

assert.commandWorked(secondary.getDB("admin").runCommand({replSetFreeze: 600}));

assert.commandFailedWithCode(
    master.getDB("admin").runCommand({replSetFreeze: 30}),
    ErrorCodes.NotSecondary,
    'replSetFreeze should return error when run on primary ' + master.host);

jsTestLog('3: step down primary ' + master.host);
try {
    master.getDB("admin").runCommand({replSetStepDown: 10, force: 1});
} catch (e) {
    print(e);
}
reconnect(master);
printjson(master.getDB("admin").runCommand({replSetGetStatus: 1}));

jsTestLog('4: freeze stepped down primary ' + master.host + ' for 30 seconds');
var start = (new Date()).getTime();
assert.commandWorked(master.getDB("admin").runCommand({replSetFreeze: 30}));

jsTestLog('5: check no one is master for 30 seconds');
while ((new Date()).getTime() - start <
       (28 * 1000)) {  // we need less 30 since it takes some time to return... hacky
    var result = master.getDB("admin").runCommand({isMaster: 1});
    assert.eq(result.ismaster, false);
    assert.eq(result.primary, undefined);
    sleep(1000);
}

jsTestLog('6: check for new primary');
var newPrimary = replTest.getPrimary();
assert.eq(master.host,
          newPrimary.host,
          'new primary should be the same node as primary that previously stepped down');

jsTestLog('7: step down new master ' + master.host);
try {
    master.getDB("admin").runCommand({replSetStepDown: 10, force: 1});
} catch (e) {
    jsTestLog('step down command threw exception' + e);
}
reconnect(master);

jsTestLog('8: freeze stepped down primary ' + master.host + ' for 30 seconds');
master.getDB("admin").runCommand({replSetFreeze: 30});
sleep(1000);

jsTestLog('9: unfreeze stepped down primary ' + master.host + ' after waiting for 1 second');
master.getDB("admin").runCommand({replSetFreeze: 0});

jsTestLog('10: wait for unfrozen node ' + master.host + ' to become primary again');
newPrimary = replTest.getPrimary();
jsTestLog('Primary after unfreezing node: ' + newPrimary.host);
assert.eq(
    master.host,
    newPrimary.host,
    'new primary after unfreezing should be the same node as primary that previously stepped down');

replTest.stopSet(15);
