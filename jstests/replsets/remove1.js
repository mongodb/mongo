/* test removing a node from a replica set
 *
 * Start set with three nodes
 * Initial sync
 * Remove slave1
 * Remove slave2
 * Bring slave1 back up
 * Bring slave2 back up
 * Add them back as slave
 * Make sure everyone's secondary
 */

load("jstests/replsets/rslib.js");
var name = "removeNodes";
var host = getHostName();


print("Start set with three nodes");
var replTest = new ReplSetTest( {name: name, nodes: 3} );
var nodes = replTest.startSet();
replTest.initiate();
var master = replTest.getMaster();


print("Initial sync");
master.getDB("foo").bar.baz.insert({x:1});

replTest.awaitReplication();


print("Remove slave2");
var config = replTest.getReplSetConfig();

config.members.pop();
config.version = 2;
try {
  master.getDB("admin").runCommand({replSetReconfig:config});
}
catch(e) {
  print(e);
}
reconnect(master);


print("Remove slave1");
config.members.pop();
config.version = 3;
try {
  master.getDB("admin").runCommand({replSetReconfig:config});
}
catch(e) {
  print(e);
}
reconnect(master);

print("clear slave ports");
// these are already down, but this clears their ports from memory so that they
// can be restarted later
replTest.stop(1);
replTest.stop(2);

print("Bring slave1 back up");
replTest.restart(1, {"fastsync":null});

print("Bring slave2 back up");
replTest.restart(2, {"fastsync":null});

print("Add them back as slaves");
config.members.push({_id:1, host : host+":"+replTest.getPort(1)});
config.members.push({_id:2, host : host+":"+replTest.getPort(2)});
config.version = 4;
printjson(config);
wait(function() {
    try {
      master.getDB("admin").runCommand({replSetReconfig:config});
    }
    catch(e) {
      print(e);
    }
    reconnect(master);

    printjson(master.getDB("admin").runCommand({replSetGetStatus:1}));
    master.setSlaveOk();
    var newConfig = master.getDB("local").system.replset.findOne();
    print( "newConfig: " + tojson(newConfig) );
    return newConfig.version == 4;
} , "wait1" );


print("Make sure everyone's secondary");
wait(function() {
    var status = master.getDB("admin").runCommand({replSetGetStatus:1});
    occasionally(function() {
        printjson(status);
      });
    
    if (!status.members || status.members.length != 3) {
      return false;
    }

    for (var i = 0; i<3; i++) {
      if (status.members[i].state != 1 && status.members[i].state != 2) {
        return false;
      }
    }
    return true;
} , "wait2" );

replTest.stopSet();

