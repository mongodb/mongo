/* test initial sync options
 *
 * Make sure member can't sync from a member with a different buildIndexes setting.
 *
 * If all nodes in a replica set are using an ephemeral storage engine, the set will not be able to
 * survive a scenario where all index-building members are down simultaneously. In such a
 * scenario, none of the members will have any data, and upon restart will each look for a member to
 * inital sync from. They cannot sync from a member which does not build indexes, so no primary will
 * be elected. This test induces such a scenario, so cannot be run on ephemeral storage engines.
 * @tags: [requires_persistence]
 */

load("jstests/replsets/rslib.js");
var name = "initialsync3";
var host = getHostName();

print("Start set with three nodes");
var replTest = new ReplSetTest({name: name, nodes: 3});
var nodes = replTest.startSet();
replTest.initiate({
    _id: name,
    members: [
        {_id: 0, host: host + ":" + nodes[0].port, priority: 10},
        {_id: 1, host: host + ":" + nodes[1].port, priority: 0},
        {_id: 2, host: host + ":" + nodes[2].port, priority: 0, buildIndexes: false},
    ]
});

var master = replTest.getPrimary();

print("Initial sync");
master.getDB("foo").bar.baz.insert({x: 1});
replTest.awaitReplication();

replTest.stop(0);
replTest.stop(1);

print("restart 1, clearing its data directory so it has to resync");
replTest.start(1);

print("make sure 1 does not become a secondary (because it cannot clone from 2)");
sleep(10000);
var result = nodes[1].getDB("admin").runCommand({isMaster: 1});
assert(!result.ismaster, tojson(result));
assert(!result.secondary, tojson(result));

print("bring 0 back up");
replTest.restart(0);
print("0 should become primary");
master = replTest.getPrimary();

print("now 1 should be able to initial sync");
assert.soon(function() {
    var result = nodes[1].getDB("admin").runCommand({isMaster: 1});
    printjson(result);
    return result.secondary;
});

replTest.stopSet();
