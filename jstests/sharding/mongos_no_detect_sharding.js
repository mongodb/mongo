// Tests whether new sharding is detected on insert by mongos
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

let st = new ShardingTest({name: "mongos_no_detect_sharding", shards: 1, mongos: 2});

let mongos = st.s;
let config = mongos.getDB("config");

print("Creating unsharded connection...");

let mongos2 = st._mongos[1];

let coll = mongos2.getCollection("test.foo");
assert.commandWorked(coll.insert({i: 0}));

print("Sharding collection...");

let admin = mongos.getDB("admin");

assert(!FixtureHelpers.isSharded(coll));

admin.runCommand({enableSharding: "test"});
admin.runCommand({shardCollection: "test.foo", key: {_id: 1}});

print("Seeing if data gets inserted unsharded...");
print("No splits occur here!");

// Insert a bunch of data which should trigger a split
let bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < 100; i++) {
    bulk.insert({i: i + 1});
}
assert.commandWorked(bulk.execute());

st.printShardingStatus(true);

assert(FixtureHelpers.isSharded(coll));
assert.eq(101, coll.find().itcount());

st.stop();
