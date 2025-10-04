//
// Tests sharding-related batch write protocol functionality
// NOTE: Basic write functionality is tested via the passthrough tests, this file should contain
// *only* mongos-specific tests.
//

import {ShardingTest} from "jstests/libs/shardingtest.js";

// Checking UUID and index consistency involves talking to the config server primary, but there is
// no config server primary by the end of this test.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;
TestData.skipCheckingIndexesConsistentAcrossCluster = true;
TestData.skipCheckOrphans = true;
TestData.skipCheckShardFilteringMetadata = true;

// The routing table consistency check runs with 'snapshot' level readConcern. This readConcern
// level cannot be satisfied without a replica set primary, which we won't have on the config server
// because this test removes the config server primary.
TestData.skipCheckRoutingTableConsistency = true;

let st = new ShardingTest({
    shards: 2,
    config: 3,
    // By default, our test infrastructure sets the election timeout to a very high value (24
    // hours). For this test, we need a shorter election timeout because it relies on nodes running
    // an election when they do not detect an active primary. Therefore, we are setting the
    // electionTimeoutMillis to its default value.
    initiateWithDefaultElectionTimeout: true,
});

jsTest.log("Starting sharding batch write tests...");

let request;
var result;

// NOTE: ALL TESTS BELOW SHOULD BE SELF-CONTAINED, FOR EASIER DEBUGGING

//
//
// Mongos _id autogeneration tests for sharded collections

let coll = st.s.getCollection("foo.bar");
assert.commandWorked(st.s.adminCommand({enableSharding: coll.getDB().toString(), primaryShard: st.shard1.shardName}));
assert.commandWorked(st.s.adminCommand({shardCollection: coll.toString(), key: {_id: 1}}));

//
// Basic insert no _id
coll.remove({});
printjson(
    (request = {
        insert: coll.getName(),
        documents: [{a: 1}],
    }),
);
printjson((result = coll.runCommand(request)));
assert(result.ok);
assert.eq(1, result.n);
assert.eq(1, coll.count());

//
// Multi insert some _ids
coll.remove({});
printjson(
    (request = {
        insert: coll.getName(),
        documents: [{_id: 0, a: 1}, {a: 2}],
    }),
);
printjson((result = coll.runCommand(request)));
assert(result.ok);
assert.eq(2, result.n);
assert.eq(2, coll.count());
assert.eq(1, coll.count({_id: 0}));

//
// Ensure generating many _ids don't push us over limits
let maxDocSize = (16 * 1024 * 1024) / 1000;
let baseDocSize = Object.bsonsize({a: 1, data: ""});
let dataSize = maxDocSize - baseDocSize;

let data = "";
for (var i = 0; i < dataSize; i++) data += "x";

let documents = [];
for (var i = 0; i < 1000; i++) documents.push({a: i, data: data});

assert.commandWorked(coll.getMongo().adminCommand({setParameter: 1, logLevel: 4}));
coll.remove({});
request = {
    insert: coll.getName(),
    documents: documents,
};
printjson((result = coll.runCommand(request)));
assert(result.ok);
assert.eq(1000, result.n);
assert.eq(1000, coll.count());

//
//
// Config server upserts (against admin db, for example) require _id test
let adminColl = st.s.getDB("admin")[coll.getName()];

//
// Without _id
adminColl.remove({});
printjson(
    (request = {
        update: adminColl.getName(),
        updates: [{q: {a: 1}, u: {a: 1}, upsert: true}],
    }),
);
var result = adminColl.runCommand(request);
assert.commandWorked(result);
assert.eq(1, result.n);
assert.eq(1, adminColl.count());

//
// With _id
adminColl.remove({});
printjson(
    (request = {
        update: adminColl.getName(),
        updates: [{q: {_id: 1, a: 1}, u: {a: 1}, upsert: true}],
    }),
);
assert.commandWorked(adminColl.runCommand(request));
assert.eq(1, result.n);
assert.eq(1, adminColl.count());

//
//
// Tests against config server
let configColl = st.s.getCollection("config.batch_write_protocol_sharded");

//
// Basic config server insert
configColl.remove({});
printjson(
    (request = {
        insert: configColl.getName(),
        documents: [{a: 1}],
    }),
);
var result = configColl.runCommand(request);
assert.commandWorked(result);
assert.eq(1, result.n);

st.configRS.awaitReplication();
assert.eq(1, st.config0.getCollection(configColl + "").count());
assert.eq(1, st.config1.getCollection(configColl + "").count());
assert.eq(1, st.config2.getCollection(configColl + "").count());

//
// Basic config server update
configColl.remove({});
configColl.insert({a: 1});
printjson(
    (request = {
        update: configColl.getName(),
        updates: [{q: {a: 1}, u: {$set: {b: 2}}}],
    }),
);
printjson((result = configColl.runCommand(request)));
assert(result.ok);
assert.eq(1, result.n);

st.configRS.awaitReplication();
assert.eq(1, st.config0.getCollection(configColl + "").count({b: 2}));
assert.eq(1, st.config1.getCollection(configColl + "").count({b: 2}));
assert.eq(1, st.config2.getCollection(configColl + "").count({b: 2}));

//
// Basic config server delete
configColl.remove({});
configColl.insert({a: 1});
printjson(
    (request = {
        "delete": configColl.getName(),
        deletes: [{q: {a: 1}, limit: 0}],
    }),
);
printjson((result = configColl.runCommand(request)));
assert(result.ok);
assert.eq(1, result.n);

st.configRS.awaitReplication();
assert.eq(0, st.config0.getCollection(configColl + "").count());
assert.eq(0, st.config1.getCollection(configColl + "").count());
assert.eq(0, st.config2.getCollection(configColl + "").count());

st.stopConfigServer(1);
st.stopConfigServer(2);
st.configRS.awaitNoPrimary();

// Config server insert with no config PRIMARY
configColl.remove({});
printjson(
    (request = {
        insert: configColl.getName(),
        documents: [{a: 1}],
    }),
);
printjson((result = configColl.runCommand(request)));
assert.commandFailedWithCode(result, ErrorCodes.FailedToSatisfyReadPreference);

// Config server insert with no config PRIMARY
configColl.remove({});
configColl.insert({a: 1});
printjson(
    (request = {
        update: configColl.getName(),
        updates: [{q: {a: 1}, u: {$set: {b: 2}}}],
    }),
);
printjson((result = configColl.runCommand(request)));
assert.commandFailedWithCode(result, ErrorCodes.FailedToSatisfyReadPreference);

// Config server insert with no config PRIMARY
configColl.remove({});
configColl.insert({a: 1});
printjson(
    (request = {
        delete: configColl.getName(),
        deletes: [{q: {a: 1}, limit: 0}],
    }),
);
printjson((result = configColl.runCommand(request)));
assert.commandFailedWithCode(result, ErrorCodes.FailedToSatisfyReadPreference);

jsTest.log("DONE!");
st.stop();
