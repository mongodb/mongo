

var replTest = new ReplSetTest({ name: 'testSet', nodes: 2 });
var nodes = replTest.startSet();
replTest.initiate();
var master = replTest.getMaster();

// do a write
master.getDB("foo").bar.insert({x:1});
replTest.awaitReplication();

// lock secondary
var locked = replTest.liveNodes.slaves[0];
locked.getDB("admin").runCommand({fsync : 1, lock : 1});

print("waiting 11 seconds");

for (var i = 0; i < 11; i++) {
    // do another write
    master.getDB("foo").bar.insert({x:i});
    sleep(1000);
}

// this should fail, so we don't need to try/catch
var result = master.getDB("admin").runCommand({replSetStepDown: 10});
printjson(result);
assert.eq(result.ok, 0);

try {
    master.getDB("admin").runCommand({replSetStepDown: 50, force : true});
}
catch (e) {
    print(e);
}

var r2 = master.getDB("admin").runCommand({ismaster : 1});
assert.eq(r2.ismaster, false);
assert.eq(r2.secondary, true);

print("unlock");
printjson(locked.getDB("admin").$cmd.sys.unlock.findOne());

print("reset stepped down time");
master.getDB("admin").runCommand({replSetFreeze:0});
master = replTest.getMaster();

print("make config with priorities");
var config = master.getDB("local").system.replset.findOne();
config.version++;
config.members[0].priority = 2;
config.members[1].priority = 1;
try {
    master.getDB("admin").runCommand({replSetReconfig : config});
}
catch (e) {
    print(e);
}
replTest.awaitReplication();

master = replTest.getMaster();
var firstMaster = master;
print("master is now "+firstMaster);

try {
    printjson(master.getDB("admin").runCommand({replSetStepDown : 100, force : true}));
}
catch (e) {
    print(e);
}

print("get a master");
replTest.getMaster();

assert.soon(function() {
        var secondMaster = replTest.getMaster();
        return firstMaster+"" != secondMaster+"";
    }, 'making sure '+firstMaster+' isn\'t still master', 60000);

replTest.stopSet();
