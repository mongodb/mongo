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
