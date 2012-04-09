/* test removing a node from a replica set
 *
 * Start set with two nodes
 * Initial sync
 * Remove secondary
 * Bring secondary back up
 * Add it back as secondary
 * Make sure both nodes are either primary or secondary
 */

load("jstests/replsets/rslib.js");
var name = "removeNodes";
var host = getHostName();

print("Start set with two nodes");
var replTest = new ReplSetTest( {name: name, nodes: 2} );
var nodes = replTest.startSet();
replTest.initiate();
var master = replTest.getMaster();

print("Initial sync");
master.getDB("foo").bar.baz.insert({x:1});

replTest.awaitReplication();

print("Remove secondary");
var config = replTest.getReplSetConfig();

config.members.pop();
config.version = 2;

assert.eq(replTest.nodes[1].getDB("admin").runCommand({ping:1}).ok, 1, "we are connected to node[1]");

try {
    master.getDB("admin").runCommand({replSetReconfig:config});
}
catch(e) {
    print(e);
}

assert.throws(replTest.nodes[1].getDB("admin").runCommand({ping:1}).ok, 1, "we are not connected to node[1]");
assert.eq(replTest.nodes[1].getDB("admin").runCommand({ping:1}).ok, 1, "we are connected to node[1]");

reconnect(master);

assert.soon(function() {
        var c = master.getDB("local").system.replset.findOne();
        return c.version == 2;
});

print("Add it back as a secondary");
config.members.push({_id:1, host : host+":"+replTest.getPort(1)});
config.version = 3;
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
    return newConfig.version == 3;
} , "wait1" );

print("Make sure both nodes are either primary or secondary");
wait(function() {
    var status = master.getDB("admin").runCommand({replSetGetStatus:1});
    occasionally(function() {
        printjson(status);
      });

    if (!status.members || status.members.length != 2) {
      return false;
    }

    for (var i = 0; i<2; i++) {
      if (status.members[i].state != 1 && status.members[i].state != 2) {
        return false;
      }
    }
    return true;
} , "wait2" );

print("reconfig with minority");
replTest.stop(1);

assert.soon(function() {
    try {
        return master.getDB("admin").runCommand({isMaster : 1}).secondary;
    }
    catch(e) {
        print("trying to get master: "+e);
    }
},"waiting for primary to step down",(60*1000),1000);

config.version = 4;
config.members.pop();
try {
    master.getDB("admin").runCommand({replSetReconfig : config, force : true});
}
catch(e) {
    print(e);
}

reconnect(master);
assert.soon(function() {
    return master.getDB("admin").runCommand({isMaster : 1}).ismaster;
},"waiting for old primary to accept reconfig and step up",(60*1000),1000);

config = master.getDB("local").system.replset.findOne();
printjson(config);
assert(config.version > 4);

replTest.stopSet();
