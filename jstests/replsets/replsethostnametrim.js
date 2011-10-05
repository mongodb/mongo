// try reconfiguring with space at the end of the host:port

var replTest = new ReplSetTest({ name: 'testSet', nodes: 1 });
var nodes = replTest.startSet();
replTest.initiate();

var master = replTest.nodes[0];
var config = master.getDB("local").system.replset.findOne();
config.version++;
var origHost = config.members[0].host;
config.members[0].host = origHost + " ";
var result = master.adminCommand({replSetReconfig : config});
assert.eq(result.ok, 0);
print("current (bad) config:"); printjson(config);

//check new config
config = master.getDB("local").system.replset.findOne();
assert.eq(origHost, config.members[0].host);
print("current (good) config:"); printjson(config);

replTest.stopSet();