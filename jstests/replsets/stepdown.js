

var replTest = new ReplSetTest({ name: 'testSet', nodes: 2 });
var nodes = replTest.startSet();
replTest.initiate();
var master = replTest.getMaster();

// do a write
master.getDB("foo").bar.insert({x:1});
replTest.awaitReplication();

// lock secondary
replTest.liveNodes.slaves[0].getDB("admin").runCommand({fsync : 1, lock : 1});

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

replTest.stopSet();
