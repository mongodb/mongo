/*
 * Test that replSetInitiate prohibits w:0 in getLastErrorDefaults when
 * a primary is on an old version and the secondary is latest, SERVER-13055.
 */

load('./jstests/multiVersion/libs/multi_rs.js');

var oldVersion = '2.6';
var newVersion = 'latest';

jsTestLog('Start one-member set with old version.');

var replTest = new ReplSetTest({nodes: {
    n0: {binVersion: oldVersion}}});

var conns = replTest.startSet();
var conf = replTest.getReplSetConfig();

conf.settings = {getLastErrorDefaults: {w: 0}};
printjson(conf);
replTest.initiate(conf);

// Wait for a primary node....
var primary = replTest.getPrimary();

jsTestLog('Add secondary running new version.');

var newSecondary = replTest.start(1, {binVersion: newVersion});
var conf2 = conns[0].getDB('local').system.replset.findOne();

printjson(newSecondary.host);

conf2.members.push({_id: 1, host: newSecondary.host});
conf2.version++;

jsTestLog('Attempting to set new config:');
printjson(conf2);

var admin = conns[0].getDB("admin");
var response = admin.runCommand({replSetReconfig: conf2});
printjson(response);

assert.soon(function() {
    try {
        var status = replTest.status();
        return (status.members.length == 2
            && status.members[1].state == replTest.DOWN
            && (/configuration is invalid/i).test(status.members[1].lastHeartbeatMessage));
    } catch (exc) {
        // Not ready.
        print(exc);
        return false;
    }
});

// Primary should step down -- probably already has.
replTest.waitForState(primary, replTest.SECONDARY);

// Since we added the new secondary directly we must stop it directly.
MongoRunner.stopMongod(newSecondary.port);

// Stop the primary.
replTest.stopSet();
