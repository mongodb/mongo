load('jstests/replsets/rslib.js');
var st = new ShardingTest({ shards: { rs0: { nodes: 2, oplogSize: 10, verbose: 1 }}});
var replTest = st.rs0;

var config = replTest.getReplSetConfig();
config.members[1].priority = 0;
// Add a delay long enough so getLastError would actually 'wait' for write concern.
config.members[1].slaveDelay = 3;
config.version = 2;

var priConn = replTest.getPrimary();

reconfig(replTest, config, true);

assert.soon(function() {
    var secConn = replTest.getSecondary();
    var config = secConn.getDB('local').system.replset.findOne();
    return config.members[1].slaveDelay == 3;
});

replTest.awaitSecondaryNodes();

var testDB = st.s.getDB('test');
testDB.adminCommand({ connPoolSync: 1 });

var secConn = replTest.getSecondary();
var testDB2 = secConn.getDB('test');

testDB.user.insert({ x: 1 });

testDB.user.ensureIndex({ x: 1 });
assert.gleOK(testDB.runCommand({ getLastError: 1, w: 2 }));

var priIdx = testDB.user.getIndexes();
var secIdx = testDB2.user.getIndexes();

assert.eq(priIdx.length, secIdx.length, 'pri: ' + tojson(priIdx) + ', sec: ' + tojson(secIdx));

st.stop();

