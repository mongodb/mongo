/*
 * Test that replSetReconfig prohibits w:0 in getLastErrorDefaults,
 * SERVER-13055.
 */

var NewReplicaSetConfigurationIncompatible = 103;

var replTest = new ReplSetTest({name: 'prohibit_w0', nodes: 1});
var nodes = replTest.nodeList();
var conns = replTest.startSet();
var admin = conns[0].getDB("admin");

replTest.initiate({_id: 'prohibit_w0', members: [{_id: 0, host: nodes[0]}]});

function testReconfig(gleDefaults) {
    var conf = admin.runCommand({replSetGetConfig: 1}).config;
    jsTestLog('conf');
    printjson(conf);
    conf.settings = gleDefaults;
    conf.version++;

    var response = admin.runCommand({replSetReconfig: conf});
    assert.commandFailedWithCode(response, NewReplicaSetConfigurationIncompatible);
}

/*
 * Try to reconfig with w: 0 in getLastErrorDefaults.
 */
testReconfig({getLastErrorDefaults: {w: 0}});

/*
 * Try to reconfig with w: 0 and other options in getLastErrorDefaults.
 */
testReconfig({getLastErrorDefaults: {w: 0, j: false, wtimeout: 100, fsync: true}});

replTest.stopSet();
