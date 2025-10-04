import {ShardingTest} from "jstests/libs/shardingtest.js";

function debug(str) {
    print("---\n" + str + "\n-----");
}

let name = "badNonUpdate";
debug("Starting sharded cluster test stuff");

let s = new ShardingTest({name: name, shards: 2, mongos: 2});

let mongosA = s.s0;
let mongosB = s.s1;

let ns = "test.coll";

let adminSA = mongosA.getDB("admin");
adminSA.runCommand({enableSharding: "test", primaryShard: s.shard0.shardName});
adminSA.runCommand({enableSharding: "test2", primaryShard: s.shard1.shardName});

adminSA.runCommand({shardCollection: ns, key: {_id: 1}});

try {
    s.stopBalancer();
} catch (e) {
    print("coundn't stop balancer via command");
}

adminSA.settings.update({_id: "balancer"}, {$set: {stopped: true}});

var db = mongosA.getDB("test");
let coll = db.coll;
let coll2 = db.coll2;

let numDocs = 10;
for (let i = 1; i < numDocs; i++) {
    coll.insert({_id: i, control: 0});
    coll2.insert({_id: i, control: 0});
}

debug("Inserted docs, now split chunks");

adminSA.runCommand({split: ns, find: {_id: 3}});
adminSA.runCommand({movechunk: ns, find: {_id: 10}, to: "s.shard1.shardName"});

let command = 'printjson(db.coll.update({ _id: 9 }, { $set: { a: "9" }}, true));';

// without this first query through mongo, the second time doesn't "fail"
debug("Try query first time");
runMongoProgram("mongo", "--quiet", "--port", "" + s._mongos[1].port, "--eval", command);

let res = mongosB.getDB("test").coll2.update({_id: 0}, {$set: {c: "333"}});
assert.eq(0, res.nModified);

s.stop();
