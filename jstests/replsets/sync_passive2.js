/**
 * Test syncing from non-primaries.
 */

load("jstests/replsets/rslib.js");

if (false) { // Test disabled until SERVER-10341 is resolved

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
assert.eq(result.errmsg, "I cannot sync from myself");

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

/**
 * Test forcing a primary to sync from another member
 */

print("sync a primary from another member: error");
result = replTest.nodes[0].getDB("admin").runCommand({replSetSyncFrom: replTest.host+":"+replTest.ports[2]});
printjson(result);
assert.eq(result.ok, 0);
assert.eq(result.errmsg, "primaries don't sync");

/**
 * Check sync target re-evaluation:
 * - Set member 3 to be slave delayed by 40 seconds.
 * - Force 2 to sync from 3
 * - Do some writes
 * - Check who 2 is syncing from, it should be someone other than three after ~30 seconds
 */
print("check sync target re-evaluation");
master = replTest.getMaster();
config = master.getDB("local").system.replset.findOne();
config.members[3].slaveDelay = 40;
config.members[3].priority = 0;
config.version++;
try {
    replTest.getMaster().getDB("admin").runCommand({replSetReconfig:config});
} catch (x) { /* expected */ }

replTest.awaitReplication(60000);
printjson(replTest.status());

print("force 2 to sync from 3");
// This briefly causes 2 to sync from 3, but members have a strong prefrence for not syncing from
// a slave delayed node so it may switch to another sync source quickly.
result = replTest.nodes[2].getDB("admin")
    .runCommand({replSetSyncFrom: replTest.host+":"+replTest.ports[3]});
printjson(result);
assert.eq(1, result.ok);

print("do writes and check that 2 changes sync targets");
assert.soon(function() {
    replTest.getMaster().getDB("foo").bar.insert({x:1});
    return !checkSyncingFrom(nodes[2], replTest.host+":"+replTest.ports[3]);
}, 'failed to change sync target', 60000);

replTest.stopSet();

} // end test disabled
