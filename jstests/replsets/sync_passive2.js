/**
 * Test syncing from non-primaries.
 */

load("jstests/replsets/rslib.js");

var name = "sync_passive2";
var host = getHostName();
  
var replTest = new ReplSetTest( {name: name, nodes: 5} );
var nodes = replTest.startSet();

// 0: master
// 1: arbiter
// 2: slave a
// 3: slave b
// 4: slave c
var config = replTest.getReplSetConfig();
config.members[1].arbiterOnly = true;
for (i=2; i<config.members.length; i++) {
    config.members[i].priority = 0;
    if (i == 4) {
        config.members[i].buildIndexes = false;
        config.members[i].hidden = true;
    }
}
replTest.initiate(config);

var master = replTest.getMaster().getDB("test");

print("Initial sync");
for (var i=0;i<10000;i++) {
    master.foo.insert({x:i, foo:"bar", msg : "all the talk on the market", date : [new Date(), new Date(), new Date()]});
}
replTest.awaitReplication();


print("stop c");
replTest.stop(4);


print("add data");
for (var i=0;i<10000;i++) {
    master.foo.insert({x:i, foo:"bar", msg : "all the talk on the market", date : [new Date(), new Date(), new Date()]});
}
replTest.awaitReplication();


print("stop b");
replTest.stop(3);


print("add data");
for (var i=0;i<10000;i++) {
    master.foo.insert({x:i, foo:"bar", msg : "all the talk on the market", date : [new Date(), new Date(), new Date()]});
}
replTest.awaitReplication();


print("kill master");
replTest.stop(0);
replTest.stop(2);


// now we have just the arbiter up

print("restart c");
replTest.restart(4);
print("restart b");
replTest.restart(3);


print("wait for sync");
wait(function() {
        var status = replTest.liveNodes.slaves[0].getDB("admin").runCommand({replSetGetStatus:1});
        occasionally(function() {
                printjson(status);
                print("1: " + status.members +" 2: "+(status.members[3].state == 2)+" 3: "+ (status.members[4].state == 2)
                      + " 4: "+friendlyEqual(status.members[3].optime, status.members[4].optime));
            });
        
        return status.members &&
            status.members[3].state == 2 &&
            status.members[4].state == 2 &&
            friendlyEqual(status.members[3].optime, status.members[4].optime);
  });


print("restart a");
replTest.restart(2);
print("wait for sync2");
wait(function() {
        var status = replTest.liveNodes.slaves[0].getDB("admin").runCommand({replSetGetStatus:1});
        occasionally(function() {
                printjson(status);
                print("1: " + status.members +" 2a: "+(status.members[3].state == 2)+" 2: "+
                      (status.members[3].state == 2)+" 3: "+ (status.members[4].state == 2)
                      + " 4: "+friendlyEqual(status.members[3].optime, status.members[4].optime));
            });

        return status.members &&
            status.members[2].state == 2 &&
            status.members[3].state == 2 &&
            status.members[4].state == 2 &&
            friendlyEqual(status.members[3].optime, status.members[4].optime) &&
            friendlyEqual(status.members[2].optime, status.members[4].optime);
  });

print("bring master back up, make sure everything's okay");
replTest.restart(0);

print("wait for sync");
wait(function() {
        var status = replTest.liveNodes.slaves[0].getDB("admin").runCommand({replSetGetStatus:1});
        occasionally(function() {
                printjson(status);
            });
        return status.members &&
            status.members[2].state == 2 &&
            status.members[3].state == 2 &&
            status.members[4].state == 2 &&
            friendlyEqual(status.members[3].optime, status.members[4].optime) &&
            friendlyEqual(status.members[2].optime, status.members[4].optime);
  });


print("force sync from various members");
master = replTest.getMaster();

print("sync from self: error");
var result = replTest.nodes[3].getDB("admin").runCommand({replSetSyncFrom: replTest.host+":"+replTest.ports[3]});
printjson(result);
assert.eq(result.ok, 0);

print("sync from arbiter: error");
result = replTest.nodes[3].getDB("admin").runCommand({replSetSyncFrom: replTest.host+":"+replTest.ports[1]});
printjson(result);
assert.eq(result.ok, 0);

print("sync arbiter from someone: error");
result = replTest.nodes[1].getDB("admin").runCommand({replSetSyncFrom: replTest.host+":"+replTest.ports[3]});
printjson(result);
assert.eq(result.ok, 0);

print("sync from non-index-building member: error");
result = replTest.nodes[3].getDB("admin").runCommand({replSetSyncFrom: replTest.host+":"+replTest.ports[4]});
printjson(result)
assert.eq(result.ok, 0);

var checkSyncingFrom = function(node, target) {
    var status = node.getDB("admin").runCommand({replSetGetStatus:1});
    occasionally(function() {
        printjson(status);
    });
    return status.syncingTo == target;
}

print("sync from primary");
result = replTest.nodes[3].getDB("admin").runCommand({replSetSyncFrom: replTest.host+":"+replTest.ports[0]});
printjson(result)
assert.eq(result.ok, 1);
assert.soon(function() {
    return checkSyncingFrom(nodes[3], replTest.host+":"+replTest.ports[0])
});

print("sync from another passive");
result = replTest.nodes[3].getDB("admin").runCommand({replSetSyncFrom: replTest.host+":"+replTest.ports[2]});
printjson(result)
assert.eq(result.ok, 1);
assert.soon(function() {
    return checkSyncingFrom(nodes[3], replTest.host+":"+replTest.ports[2])
});

print("sync primary on stepdown");
result = replTest.nodes[0].getDB("admin").runCommand({replSetSyncFrom: replTest.host+":"+replTest.ports[3]});
printjson(result)
assert.eq(result.ok, 1);

try {
    replTest.nodes[0].getDB("admin").runCommand({replSetStepDown:60});
}
catch (e) {
    print(e);
}

// hammer this, because it's probably not going to stick to it for long
assert.soon(function() {
    return checkSyncingFrom(nodes[0], replTest.host+":"+replTest.ports[3])
}, "stepdown", 30000, 0);

replTest.stopSet();
