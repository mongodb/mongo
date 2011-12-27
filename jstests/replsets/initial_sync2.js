/**
 * Test killing the primary during initial sync
 * and don't allow the other secondary to become primary
 *
 * 1. Bring up set
 * 2. Insert some data
 * 4. Make sure synced
 * 5. Freeze #2
 * 6. Bring up #3
 * 7. Kill #1 in the middle of syncing
 * 8. Check that #3 makes it into secondary state
 * 9. Bring #1 back up
 * 10. Initial sync should succeed
 * 11. Insert some stuff
 * 12. Everyone happy eventually
 */

load("jstests/replsets/rslib.js");
var basename = "jstests_initsync2";

var doTest = function() {

print("1. Bring up set");
var replTest = new ReplSetTest( {name: basename, nodes: 2} );
var conns = replTest.startSet();
replTest.initiate();

var master = replTest.getMaster();
var origMaster = master;
var foo = master.getDB("foo");
var admin = master.getDB("admin");

var slave1 = replTest.liveNodes.slaves[0];
var admin_s1 = slave1.getDB("admin");
var local_s1 = slave1.getDB("local");

print("2. Insert some data");
for (var i=0; i<10000; i++) {
  foo.bar.insert({date : new Date(), x : i, str : "all the talk on the market"});
}
print("total in foo: "+foo.bar.count());


print("4. Make sure synced");
replTest.awaitReplication();


print("5. Freeze #2");
admin_s1.runCommand({replSetFreeze:999999});


print("6. Bring up #3");
var ports = allocatePorts( 3 );
var basePath = "/data/db/" + basename;
var hostname = getHostName();

var slave2 = startMongodTest (ports[2], basename, false, {replSet : basename, oplogSize : 2} )

var local_s2 = slave2.getDB("local");
var admin_s2 = slave2.getDB("admin");

var config = replTest.getReplSetConfig();
config.version = 2;
config.members.push({_id:2, host:hostname+":"+ports[2]});

try {
  admin.runCommand({replSetReconfig:config});
}
catch(e) {
  print(e);
}
reconnect(slave1);
reconnect(slave2);

wait(function() {
    var config2 = local_s1.system.replset.findOne();
    var config3 = local_s2.system.replset.findOne();

    printjson(config2);
    printjson(config3);

    return config2.version == config.version &&
      (config3 && config3.version == config.version);
  });
admin_s2.runCommand({replSetFreeze:999999});


wait(function() {
    var status = admin_s2.runCommand({replSetGetStatus:1});
    printjson(status);
    return status.members &&
      (status.members[2].state == 3 || status.members[2].state == 2);
  });


print("7. Kill #1 in the middle of syncing");
replTest.stop(0);


print("8. Check that #3 makes it into secondary state");
wait(function() {
        var status = admin_s2.runCommand({replSetGetStatus:1});
        occasionally(function() { printjson(status);}, 10);
        if (status.members[2].state == 2 || status.members[2].state == 1) {
            return true;
        }
        return false;
    });


print("9. Bring #1 back up");
replTest.start(0, {}, true);
reconnect(master);
wait(function() {
    var status = admin.runCommand({replSetGetStatus:1});
    printjson(status);
    return status.members &&
      (status.members[0].state == 1 || status.members[0].state == 2);
  });


print("10. Initial sync should succeed");
wait(function() {
    var status = admin_s2.runCommand({replSetGetStatus:1});
    printjson(status);
    return status.members &&
      status.members[2].state == 2 || status.members[2].state == 1;
  });


print("11. Insert some stuff");
// ReplSetTest doesn't find master correctly unless all nodes are defined by
// ReplSetTest
for (var i = 0; i<30; i++) {
  var result = admin.runCommand({isMaster : 1});
  if (result.ismaster) {
    break;
  }
  else if (result.primary) {
    master = connect(result.primary+"/admin").getMongo();
    break;
  }
  sleep(1000);
}

for (var i=0; i<10000; i++) {
  foo.bar.insert({date : new Date(), x : i, str : "all the talk on the market"});
}


print("12. Everyone happy eventually");
// if 3 is master...
if (master+"" != origMaster+"") {
  print("3 is master");
  slave2 = origMaster;
}

wait(function() {
    var op1 = getLatestOp(master);
    var op2 = getLatestOp(slave1);
    var op3 = getLatestOp(slave2);

    occasionally(function() {
        print("latest ops:");
        printjson(op1);
        printjson(op2);
        printjson(op3);
      });
    
    return friendlyEqual(getLatestOp(master), getLatestOp(slave1)) &&
      friendlyEqual(getLatestOp(master), getLatestOp(slave2));
  });

replTest.stopSet();
};

doTest();
