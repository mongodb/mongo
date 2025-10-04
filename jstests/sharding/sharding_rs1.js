/**
 * tests sharding with replica sets
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

let s = new ShardingTest({shards: 3, other: {rs: true, chunkSize: 2, enableBalancer: true}});

s.adminCommand({enablesharding: "test", primaryShard: s.shard0.shardName});
s.config.settings.update({_id: "balancer"}, {$set: {_waitForDelete: true}}, true);

var db = s.getDB("test");

let bigString = "X".repeat(256 * 1024); // 250 KB

let insertedBytes = 0;
let num = 0;

// Insert 20 MB of data to result in 20 chunks
let bulk = db.foo.initializeUnorderedBulkOp();
while (insertedBytes < 20 * 1024 * 1024) {
    bulk.insert({_id: num++, s: bigString, x: Math.random()});
    insertedBytes += bigString.length;
}
assert.commandWorked(bulk.execute({w: 3}));

assert.commandWorked(s.s.adminCommand({shardcollection: "test.foo", key: {_id: 1}}));

jsTest.log("Waiting for balance to complete");
s.awaitBalance("foo", "test", 5 * 60 * 1000);

jsTest.log("Stopping balancer");
s.stopBalancer();

jsTest.log("Balancer stopped, checking dbhashes");
s._rs.forEach(function (rsNode) {
    rsNode.test.awaitReplication();

    let dbHashes = rsNode.test.getHashes("test");
    print(rsNode.url + ": " + tojson(dbHashes));

    for (let j = 0; j < dbHashes.secondaries.length; j++) {
        assert.eq(
            dbHashes.primary.md5,
            dbHashes.secondaries[j].md5,
            "hashes not same for: " + rsNode.url + " secondary: " + j,
        );
    }
});

assert.eq(num, db.foo.find().count(), "C1");
assert.eq(num, db.foo.find().itcount(), "C2");
assert.eq(num, db.foo.find().sort({_id: 1}).itcount(), "C3");
assert.eq(num, db.foo.find().sort({_id: -1}).itcount(), "C4");

db.foo.createIndex({x: 1});
assert.eq(num, db.foo.find().sort({x: 1}).itcount(), "C5");
assert.eq(num, db.foo.find().sort({x: -1}).itcount(), "C6");

s.stop();
