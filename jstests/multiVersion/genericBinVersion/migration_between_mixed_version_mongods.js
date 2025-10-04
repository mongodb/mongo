//
// Testing migrations between latest and last-lts mongod versions, where the
// donor is the latest version and the recipient the last-lts, and vice versa.
// Migrations should be successful.
//
import "jstests/multiVersion/libs/verify_versions.js";

import {ShardingTest} from "jstests/libs/shardingtest.js";

// Checking UUID consistency involves talking to a shard node, which in this test is shutdown
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

let options = {
    shards: [{binVersion: "last-lts"}, {binVersion: "last-lts"}, {binVersion: "latest"}, {binVersion: "latest"}],
    mongos: 1,
    other: {mongosOptions: {binVersion: "last-lts"}},
};

let st = new ShardingTest(options);
st.stopBalancer();

assert.binVersion(st.shard0, "last-lts");
assert.binVersion(st.shard1, "last-lts");
assert.binVersion(st.shard2, "latest");
assert.binVersion(st.shard3, "latest");
assert.binVersion(st.s0, "last-lts");

const fooDB = "fooTest";
const barDB = "barTest";
let mongos = st.s0,
    admin = mongos.getDB("admin"),
    shards = mongos.getCollection("config.shards").find().toArray();

assert.commandWorked(admin.runCommand({enableSharding: fooDB, primaryShard: shards[0]._id}));
assert.commandWorked(admin.runCommand({enableSharding: barDB, primaryShard: shards[3]._id}));

let fooNS = fooDB + ".foo",
    fooColl = mongos.getCollection(fooNS),
    fooDonor = st.shard0,
    fooRecipient = st.shard2,
    fooDonorColl = fooDonor.getCollection(fooNS),
    fooRecipientColl = fooRecipient.getCollection(fooNS),
    barNS = barDB + ".foo",
    barColl = mongos.getCollection(barNS),
    barDonor = st.shard3,
    barRecipient = st.shard1,
    barDonorColl = barDonor.getCollection(barNS),
    barRecipientColl = barRecipient.getCollection(barNS);

assert.commandWorked(admin.runCommand({shardCollection: fooNS, key: {a: 1}}));
assert.commandWorked(admin.runCommand({split: fooNS, middle: {a: 10}}));
assert.commandWorked(admin.runCommand({shardCollection: barNS, key: {a: 1}}));
assert.commandWorked(admin.runCommand({split: barNS, middle: {a: 10}}));

assert.commandWorked(fooColl.insert({a: 0}));
assert.commandWorked(fooColl.insert({a: 10}));
assert.eq(0, fooRecipientColl.count());
assert.eq(2, fooDonorColl.count());
assert.eq(2, fooColl.count());

assert.commandWorked(barColl.insert({a: 0}));
assert.commandWorked(barColl.insert({a: 10}));
assert.eq(0, barRecipientColl.count());
assert.eq(2, barDonorColl.count());
assert.eq(2, barColl.count());

/**
 * Perform two migrations:
 *      shard0 (last-lts) -> foo chunk -> shard2 (latest)
 *      shard3 (latest)      -> bar chunk -> shard1 (last-lts)
 */

assert.commandWorked(admin.runCommand({moveChunk: fooNS, find: {a: 10}, to: shards[2]._id, _waitForDelete: true}));
assert.commandWorked(admin.runCommand({moveChunk: barNS, find: {a: 10}, to: shards[1]._id, _waitForDelete: true}));
assert.eq(
    1,
    fooRecipientColl.count(),
    "Foo collection migration failed. " + "Last-lts -> latest mongod version migration failure.",
);
assert.eq(
    1,
    fooDonorColl.count(),
    "Foo donor lost its document. " + "Last-lts -> latest mongod version migration failure.",
);
assert.eq(
    2,
    fooColl.count(),
    "Incorrect number of documents in foo collection. " + "Last-lts -> latest mongod version migration failure.",
);
assert.eq(
    1,
    barRecipientColl.count(),
    "Bar collection migration failed. " + "Latest -> last-lts mongod version migration failure.",
);
assert.eq(
    1,
    barDonorColl.count(),
    "Bar donor lost its document. " + "Latest -> last-lts mongod version migration failure.",
);
assert.eq(
    2,
    barColl.count(),
    "Incorrect number of documents in bar collection. " + "Latest -> last-lts mongod version migration failure.",
);

st.stop();
