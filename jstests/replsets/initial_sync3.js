/* test initial sync options
 *
 * Make sure member can't sync from a member with a different buildIndexes setting.
 */


load("jstests/replsets/rslib.js");
var name = "initialsync3";
var host = getHostName();
var port = allocatePorts(7);

print("Start set with three nodes");
var replTest = new ReplSetTest( {name: name, nodes: 3} );
var nodes = replTest.startSet();
replTest.initiate({
    _id : name,
    members : [
        {_id:0, host : host+":"+port[0]},
        {_id:1, host : host+":"+port[1]},
        {_id:2, host : host+":"+port[2], priority : 0, buildIndexes : false},
               ]});

var master = replTest.getMaster();

print("Initial sync");
master.getDB("foo").bar.baz.insert({x:1});
replTest.awaitReplication();

replTest.stop(0);
replTest.stop(1);

print("restart 1, clearing its data directory so it has to resync");
replTest.start(1);

print("make sure 1 does not become a secondary (because it cannot clone from 2)");
sleep(10000);
var result = nodes[1].getDB("admin").runCommand({isMaster : 1});
assert(!result.ismaster, tojson(result));
assert(!result.secondary, tojson(result));

print("bring 0 back up");
replTest.restart(0);
print("0 should become primary");
master = replTest.getMaster();

print("now 1 should be able to initial sync");
assert.soon(function() {
    var result = nodes[1].getDB("admin").runCommand({isMaster : 1});
    printjson(result);
    return result.secondary;
});

replTest.stopSet();

print("reconfig");

var rs2 = new ReplSetTest( {name: 'reconfig-isync3', nodes: 3} );
rs2.startSet();
rs2.initiate();

master = rs2.getMaster();
var config = master.getDB("local").system.replset.findOne();
config.version++;
config.members[0].priority = 2;
config.members[0].initialSync = {state : 2};
config.members[1].initialSync = {state : 1};
try {
    master.getDB("admin").runCommand({replSetReconfig : config});
}
catch(e) {
    assert((e.message || e) == "error doing query: failed");
}

// wait for a heartbeat, too, just in case sync happens before hb
assert.soon(function() {
    try {
      for (var n in rs2.nodes) {
        if (rs2.nodes[n].getDB("local").system.replset.findOne().version != 2) {
          return false;
        }
      }
    }
    catch (e) {
      return false;
    }
    return true;
});

rs2.awaitReplication();

// test partitioning
master = rs2.bridge();
rs2.partition(0, 2);

master.getDB("foo").bar.baz.insert({x:1});
rs2.awaitReplication();

master = rs2.getMaster();

master.getDB("foo").bar.baz.insert({x:2});
var x = master.getDB("foo").runCommand({getLastError : 1, w : 3, wtimeout : 60000});
printjson(x);
assert.eq(null, x.err);

rs2.stopSet();

print("initialSync3 success!");
