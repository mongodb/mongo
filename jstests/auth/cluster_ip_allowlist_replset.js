/**
 * This test verifies that setting clusterIpSourceAllowlist to non-existent IP
 * will prevent a node from joining a cluster. Then it will also verify that
 * resetting it to a good IP will allow a node to join cluster
 * @tags: [requires_replication]
 */

(function() {
'use strict';

load('jstests/replsets/rslib.js');
load("jstests/libs/log.js");
load("jstests/libs/parallelTester.js");

const kKeyFile = 'jstests/libs/key1';
const replTest = new ReplSetTest({nodes: 1, nodeOptions: {auth: ""}, keyFile: kKeyFile});
replTest.startSet();
replTest.initiate();

const primary = replTest.getPrimary();
const admin = primary.getDB('admin');
assert.commandWorked(admin.runCommand({createUser: 'admin', pwd: 'admin', roles: ['root']}));
admin.auth('admin', 'admin');

assert.commandWorked(primary.adminCommand(
    {setDefaultRWConcern: 1, defaultWriteConcern: {w: "majority"}, writeConcern: {w: "majority"}}));

function resetClusterIpSourceAllowlist(host) {
    jsTest.log("resetClusterIpSourceAllowlist: started");
    const mongo = new Mongo(host);
    const admin = mongo.getDB("admin");
    admin.auth('admin', 'admin');

    jsTest.log("Check log for denied connections");
    checkLog.containsJson(admin, 20240, {});
    jsTest.log("Found denied connection(s)");

    assert.commandWorked(admin.adminCommand({setParameter: 1, "clusterIpSourceAllowlist": null}));
    jsTest.log("Successfully reset clusterIpSourceAllowlist");
}

let thread = new Thread(resetClusterIpSourceAllowlist, primary.host);
thread.start();

const arbiterConn = replTest.add();
const conf = replTest.getReplSetConfigFromNode();
conf.members.push({_id: 2, host: arbiterConn.host, arbiterOnly: true});
conf.version++;

// Following function will succeed after resetClusterIpSourceAllowlist()
// successfully resets IP for connection
jsTest.log("ReplSet started, will block __system connections and expect error");
assert.commandWorked(
    primary.adminCommand({setParameter: 1, "clusterIpSourceAllowlist": ["192.0.2.1"]}));
assert.commandWorked(admin.runCommand({replSetReconfig: conf}));
thread.join();

jsTest.log("Verify that connections were denied");
checkLog.containsJson(admin, 20240, {}, 1);

replTest.awaitReplication();

conf.version++;
assert.commandWorked(admin.runCommand({replSetReconfig: conf}));

replTest.stopSet();
}());
