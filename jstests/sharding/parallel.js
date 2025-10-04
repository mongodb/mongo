// This test fails when run with authentication because benchRun with auth is broken: SERVER-6388
import {ShardingTest} from "jstests/libs/shardingtest.js";

let numShards = 3;
let s = new ShardingTest({name: "parallel", shards: numShards, mongos: 2});

s.adminCommand({enablesharding: "test", primaryShard: s.shard1.shardName});
s.adminCommand({shardcollection: "test.foo", key: {_id: 1}});

var db = s.getDB("test");

let N = 10000;
let shards = [s.shard0.shardName, s.shard1.shardName, s.shard2.shardName];

for (var i = 0; i < N; i += N / 10) {
    s.adminCommand({split: "test.foo", middle: {_id: i}});
    s.s
        .getDB("admin")
        .runCommand({moveChunk: "test.foo", find: {_id: i}, to: shards[Math.floor(Math.random() * numShards)]});
}

s.startBalancer();

let bulk = db.foo.initializeUnorderedBulkOp();
for (i = 0; i < N; i++) bulk.insert({_id: i});
assert.commandWorked(bulk.execute());

let doCommand = function (dbname, cmd) {
    x = benchRun({
        ops: [{op: "findOne", ns: dbname + ".$cmd", query: cmd, readCmd: true}],
        host: db.getMongo().host,
        parallel: 2,
        seconds: 2,
    });
    printjson(x);
    x = benchRun({
        ops: [{op: "findOne", ns: dbname + ".$cmd", query: cmd, readCmd: true}],
        host: s._mongos[1].host,
        parallel: 2,
        seconds: 2,
    });
    printjson(x);
};

doCommand("test", {dbstats: 1});
doCommand("config", {dbstats: 1});

var x = s.getDB("config").stats();
assert(x.ok, tojson(x));
printjson(x);

s.stop();
