/**
 * Testing migrations are successful and immediately visible on the secondaries, when
 * secondaryThrottle is used.
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

// The mongod secondaries are set to priority 0 to prevent the primaries from stepping down during
// migrations on slow evergreen builders.
let s = new ShardingTest({
    shards: 2,
    other: {
        chunkSize: 1,
        rs0: {
            nodes: [{rsConfig: {}}, {rsConfig: {priority: 0}}],
        },
        rs1: {
            nodes: [{rsConfig: {}}, {rsConfig: {priority: 0}}],
        },
    },
});

assert.commandWorked(s.s0.adminCommand({enablesharding: "TestDB", primaryShard: s.shard0.shardName}));

let bulk = s.s0.getDB("TestDB").TestColl.initializeUnorderedBulkOp();
for (var i = 0; i < 2100; i++) {
    bulk.insert({_id: i, x: i});
}
assert.commandWorked(bulk.execute({w: "majority"}));

assert.commandWorked(s.s0.adminCommand({shardcollection: "TestDB.TestColl", key: {_id: 1}}));

for (i = 0; i < 20; i++) {
    assert.commandWorked(s.s0.adminCommand({split: "TestDB.TestColl", middle: {_id: i * 100}}));
}

let collPrimary = new Mongo(s.s0.host).getDB("TestDB").TestColl;
assert.eq(2100, collPrimary.find().itcount());

let collSecondaryOk = new Mongo(s.s0.host).getDB("TestDB").TestColl;
collSecondaryOk.setSecondaryOk();
assert.eq(2100, collSecondaryOk.find().itcount());

assert.commandWorked(
    s.s0.adminCommand({
        moveChunk: "TestDB.TestColl",
        find: {_id: 0},
        to: s.shard1.shardName,
        _secondaryThrottle: true,
        writeConcern: {w: 2},
        _waitForDelete: true,
    }),
);

assert.eq(
    2100,
    collSecondaryOk.find().itcount(),
    "Incorrect count when reading from secondary. Count from primary is " + collPrimary.find().itcount(),
);

s.stop();
