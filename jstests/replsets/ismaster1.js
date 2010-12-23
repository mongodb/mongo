/**
 * 1. Check passive field in isMaster
 */

load("jstests/replsets/rslib.js");

var name = "ismaster";
var host = getHostName();

var replTest = new ReplSetTest( {name: name, nodes: 3} );

var nodes = replTest.startSet();

var config = replTest.getReplSetConfig();
config.members[1].priority = 0;
config.members[2].priority = 0;
  
replTest.initiate(config);

var master = replTest.getMaster();
wait(function() {
        var result = master.getDB("admin").runCommand({replSetGetStatus:1});
        return result.members && result.members[0].state == 1 &&
            result.members[1].state == 2 && result.members[2].state == 2;
    });

var result = master.getDB("admin").runCommand({isMaster:1});
assert(!('passive' in result), tojson(result));

result = replTest.liveNodes.slaves[0].getDB("admin").runCommand({isMaster:1});
assert('passive' in result, tojson(result));

result = replTest.liveNodes.slaves[1].getDB("admin").runCommand({isMaster:1});
assert('passive' in result, tojson(result));

replTest.stopSet();
