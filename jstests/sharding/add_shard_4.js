// A replica set's passive nodes should be okay to add as part of a shard config
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

let s = new ShardingTest({name: "addshard4", shards: 2, mongos: 1, other: {useHostname: true}});

let r = new ReplSetTest({name: "addshard4", nodes: 3, nodeOptions: {shardsvr: ""}});

r.startSet();

let config = r.getReplSetConfig();
config.members[2].priority = 0;

r.initiate(config);
// Wait for replica set to be fully initialized - could take some time
// to pre-allocate files on slow systems
r.awaitReplication();

let members = config.members.map(function (elem) {
    return elem.host;
});
let shardName = "addshard4/" + members.join(",");
let invalidShardName = "addshard4/foobar";

print("adding shard " + shardName);

// First try adding shard with the correct replica set name but incorrect hostname
// This will make sure that the metadata for this replica set name is cleaned up
// so that the set can be added correctly when it has the proper hostnames.
assert.throws(function () {
    s.adminCommand({"addshard": invalidShardName});
});

let result = s.adminCommand({"addshard": shardName});

printjson(result);
assert.eq(result, true);

let r42 = new ReplSetTest({name: "addshard42", nodes: 3, nodeOptions: {shardsvr: ""}});
r42.startSet();

config = r42.getReplSetConfig();
config.members[2].arbiterOnly = true;

r42.initiate(config);
// Wait for replica set to be fully initialized - could take some time
// to pre-allocate files on slow systems
r42.awaitReplication();

print("adding shard addshard42");

// Setting CWWC for addShard to work, as implicitDefaultWC is set to w:1.
assert.commandWorked(
    s.s.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}),
);
result = s.adminCommand({"addshard": "addshard42/" + config.members[2].host});

printjson(result);
assert.eq(result, true);

s.stop();
r.stopSet();
r42.stopSet();
