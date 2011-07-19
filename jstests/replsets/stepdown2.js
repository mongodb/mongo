print("\nstepdown2.js");

var replTest = new ReplSetTest({ name: 'testSet', nodes: 2 });
var nodes = replTest.startSet();
replTest.initiate();
var master = replTest.getMaster();

// do a write
print("\ndo a write");
master.getDB("foo").bar.insert({x:1});
replTest.awaitReplication();

// lock secondary
print("\nlock secondary");
var locked = replTest.liveNodes.slaves[0];
printjson( locked.getDB("admin").runCommand({fsync : 1, lock : 1}) );

print("\nwaiting 11ish seconds");

sleep(3003);

for (var i = 0; i < 11; i++) {
    // do another write
    master.getDB("foo").bar.insert({x:i});
    sleep(1008);
}

print("\n do stepdown that should not work");

// this should fail, so we don't need to try/catch
var result = master.getDB("admin").runCommand({replSetStepDown: 10});
printjson(result);
assert.eq(result.ok, 0);

print("\n do stepdown that should work");
try {
    master.getDB("admin").runCommand({replSetStepDown: 50, force : true});
}
catch (e) {
    print(e);
}

var r2 = master.getDB("admin").runCommand({ismaster : 1});
assert.eq(r2.ismaster, false);
assert.eq(r2.secondary, true);

print("\nunlock");
printjson(locked.getDB("admin").$cmd.sys.unlock.findOne());

print("\nreset stepped down time");
master.getDB("admin").runCommand({replSetFreeze:0});
master = replTest.getMaster();

print("\nmake 1 config with priorities");
var config = master.getDB("local").system.replset.findOne();
print("\nmake 2");
config.version++;
config.members[0].priority = 2;
config.members[1].priority = 1;
// make sure 1 can stay master once 0 is down
config.members[0].votes = 0;
try {
    master.getDB("admin").runCommand({replSetReconfig : config});
}
catch (e) {
    print(e);
}

print("\nawait");
replTest.awaitReplication();

master = replTest.getMaster();
var firstMaster = master;
print("\nmaster is now "+firstMaster);

try {
    printjson(master.getDB("admin").runCommand({replSetStepDown : 100, force : true}));
}
catch (e) {
    print(e);
}

print("\nget a master");
replTest.getMaster();

assert.soon(function() {
        var secondMaster = replTest.getMaster();
        return firstMaster+"" != secondMaster+"";
    }, 'making sure '+firstMaster+' isn\'t still master', 60000);


print("\ncheck shutdown command");

master = replTest.liveNodes.master;
var slave = replTest.liveNodes.slaves[0];
var slaveId = replTest.getNodeId(slave);

try {
    slave.adminCommand({shutdown :1})
}
catch (e) {
    print(e);
}

print("\nsleeping");

sleep(2000);

print("\nrunning shutdown without force on master: "+master);

result = replTest.getMaster().getDB("admin").runCommand({shutdown : 1, timeoutSecs : 3});
assert.eq(result.ok, 0);

print("\nsend shutdown command");

var currentMaster = replTest.getMaster();
try {
    printjson(currentMaster.getDB("admin").runCommand({shutdown : 1, force : true}));
}
catch (e) {
    print(e);
}

print("checking "+currentMaster+" is actually shutting down");
assert.soon(function() {
    try {
        currentMaster.findOne();
    }
    catch(e) {
        return true;
    }
    return false;
});

print("\nOK 1 stepdown2.js");

replTest.stopSet();

print("\nOK 2 stepdown2.js");
