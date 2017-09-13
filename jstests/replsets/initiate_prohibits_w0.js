/*
 * Test that replSetInitiate prohibits w:0 in getLastErrorDefaults,
 * SERVER-13055.
 */

var InvalidReplicaSetConfig = 93;

var replTest = new ReplSetTest({name: 'prohibit_w0', nodes: 1});
var nodes = replTest.nodeList();
var conns = replTest.startSet();
var admin = conns[0].getDB("admin");

function testInitiate(gleDefaults) {
    var conf = replTest.getReplSetConfig();
    jsTestLog('conf');
    printjson(conf);
    conf.settings = gleDefaults;

    var response = admin.runCommand({replSetInitiate: conf});
    assert.commandFailedWithCode(response, InvalidReplicaSetConfig);
}

/*
 * Try to initiate with w: 0 in getLastErrorDefaults.
 */
testInitiate({getLastErrorDefaults: {w: 0}});

/*
 * Try to initiate with w: 0 and other options in getLastErrorDefaults.
 */
testInitiate({getLastErrorDefaults: {w: 0, j: false, wtimeout: 100, fsync: true}});

replTest.stopSet();
