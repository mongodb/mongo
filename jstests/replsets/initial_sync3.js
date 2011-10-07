/* test initial sync options
 *
 * {state : 1}
 * {state : 2}
 * {name : host+":"+port}
 * {_id : 2}
 * {optime : now}
 * {optime : 1970}
 */

load("jstests/replsets/rslib.js");
var name = "initialsync3";
var host = getHostName();
var port = allocatePorts(7);

print("Start set with three nodes");
var replTest = new ReplSetTest( {name: name, nodes: 7} );
var nodes = replTest.startSet();
replTest.initiate({
    _id : name,
    members : [
               {_id:0, host : host+":"+port[0]},
               {_id:1, host : host+":"+port[1], initialSync : {state : 1}},
               {_id:2, host : host+":"+port[2], initialSync : {state : 2}},
               {_id:3, host : host+":"+port[3], initialSync : {name : host+":"+port[2]}},
               {_id:4, host : host+":"+port[4], initialSync : {_id : 2}},
               {_id:5, host : host+":"+port[5], initialSync : {optime : new Date()}},
               {_id:6, host : host+":"+port[6], initialSync : {optime : new Date(0)}}
               ]});

var master = replTest.getMaster();

print("Initial sync");
master.getDB("foo").bar.baz.insert({x:1});

print("Make sure everyone's secondary");
wait(function() {
    var status = master.getDB("admin").runCommand({replSetGetStatus:1});
    occasionally(function() {
        printjson(status);
      });

    if (!status.members) {
      return false;
    }

    for (i=0; i<7; i++) {
      if (status.members[i].state != 1 && status.members[i].state != 2) {
        return false;
      }
    }
    return true;

  });

replTest.awaitReplication();

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
    print("trying to reconfigure: "+e);
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
var x = master.getDB("foo").runCommand({getLastError : 1, w : 3, wtimeout : 5000});
printjson(x);
assert.eq(null, x.err);

rs2.stopSet();

print("initialSync3 success!");
