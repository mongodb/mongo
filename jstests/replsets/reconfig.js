
// try reconfiguring with servers down

var replTest = new ReplSetTest({ name: 'testSet', nodes: 5 });
var nodes = replTest.startSet();
replTest.initiate();

var master = replTest.getMaster();

print("initial sync");
master.getDB("foo").bar.insert({X:1});
replTest.awaitReplication();

print("stopping 3 & 4");
replTest.stop(3);
replTest.stop(4);

print("reconfiguring");
var config = master.getDB("local").system.replset.findOne();
var oldVersion = config.version++;
config.members[0].votes = 2;
config.members[3].votes = 2;
try {
    master.getDB("admin").runCommand({replSetReconfig : config});
}
catch(e) {
    print(e);
}

var config = master.getDB("local").system.replset.findOne();
assert.eq(oldVersion+1, config.version);

print("0 & 3 up; 1, 2, 4 down");
replTest.restart(3);

replTest.stop(1);
replTest.stop(2);

print("try to reconfigure with a 'majority' down");
oldVersion = config.version++;
master = replTest.getMaster();
var result = master.getDB("admin").runCommand({replSetReconfig : config});
assert.eq(13144, result.assertionCode);

var config = master.getDB("local").system.replset.findOne();
assert.eq(oldVersion, config.version);

replTest.stopSet();
