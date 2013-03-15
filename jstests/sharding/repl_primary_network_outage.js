/**
 * Test what happens in a sharded system when the primary of a replica set completely loses network
 * connectivity.
 */

function assertGLEOK(status) {
    assert(status.ok && status.err === null,
           "Expected OK status object; found " + tojson(status));
}

var NODE_COUNT = 3;
var st = new ShardingTest({ shards: { rs0: { nodes: NODE_COUNT, oplogSize: 10 }},
    separateConfig: true, config : 3 });
var replTest = st.rs0;
var mongos = st.s;
var db = mongos.getDB('test');

jsTest.log("Inserting initial data");
db.foo.insert({a:1});
assertGLEOK(db.getLastErrorObj('majority'));

jsTest.log("Activing socket exception failpoint");
// Activate failpoint to simulate network blackhole
var initialPrimary = replTest.getPrimary();
try {
    initialPrimary.getDB("admin").runCommand({configureFailPoint: 'throwSockExcep',
                                              mode: 'alwaysOn'});
} catch (ignored) {
    // Expected - there will always be a socket exception thrown after activating the failpoint.
}

jsTest.log("Waiting for new primary to be selected");

var newPrimary = replTest.getPrimary();
assert.neq(initialPrimary, newPrimary);

ReplSetTest.awaitRSClientHosts(mongos, newPrimary, { ok : true, ismaster : true });

jsTest.log("Performing new insert to verify that mongos recovered");

db.foo.insert({a:1});
assertGLEOK(db.getLastErrorObj());

assert.eq(2, db.foo.count());

st.stop();