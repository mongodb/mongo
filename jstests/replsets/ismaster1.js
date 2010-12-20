/**
 * 1. Check passive field in isMaster
 */

var name = "ismaster";
var host = getHostName();

var replTest = new ReplSetTest( {name: name, nodes: 3} );

var nodes = replTest.startSet();

var config = replTest.getReplSetConfig();
config.members[1].priority = 0;
config.members[2].priority = 0;
  
replTest.initiate(config);

var master = replTest.getMaster();
var result = master.getDB("admin").runCommand({isMaster:1});
assert(!('passive' in result));

result = replTest.liveNodes.slaves[0].getDB("admin").runCommand({isMaster:1});
assert('passive' in result);

result = replTest.liveNodes.slaves[1].getDB("admin").runCommand({isMaster:1});
assert('passive' in result);

replTest.stopSet();
