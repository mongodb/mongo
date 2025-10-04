// When checking UUID consistency, the shell attempts to run a command on the node it believes is
// primary in each shard. However, this test shuts down the primary of the shard. Since whether or
// not the shell detects the new primary before issuing the command is nondeterministic, skip the
// consistency check for this test.

TestData.skipCheckingUUIDsConsistentAcrossCluster = true;
TestData.skipCheckShardFilteringMetadata = true;

import {awaitRSClientHosts} from "jstests/replsets/rslib.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

let s = new ShardingTest({
    shards: {rs0: {nodes: [{}, {}, {rsConfig: {priority: 0}}]}},
    mongos: 1,
    other: {rs: true, chunkSize: 1},
    // By default, our test infrastructure sets the election timeout to a very high value (24
    // hours). For this test, we need a shorter election timeout because it relies on nodes running
    // an election when they do not detect an active primary. Therefore, we are setting the
    // electionTimeoutMillis to its default value.
    initiateWithDefaultElectionTimeout: true,
});

assert.commandWorked(s.s0.adminCommand({enablesharding: "test"}));
assert.commandWorked(s.s0.adminCommand({shardcollection: "test.foo", key: {_id: 1}}));

var db = s.getDB("test");

let bulk = db.foo.initializeUnorderedBulkOp();
let bulk2 = db.bar.initializeUnorderedBulkOp();
for (let i = 0; i < 100; i++) {
    bulk.insert({_id: i, x: i});
    bulk2.insert({_id: i, x: i});
}
assert.commandWorked(bulk.execute());
assert.commandWorked(bulk2.execute());

s.splitAt("test.foo", {_id: 50});

let other = new Mongo(s.s0.name);
let dbother = other.getDB("test");

assert.eq(5, db.foo.findOne({_id: 5}).x);
assert.eq(5, dbother.foo.findOne({_id: 5}).x);

assert.eq(5, db.bar.findOne({_id: 5}).x);
assert.eq(5, dbother.bar.findOne({_id: 5}).x);

s.rs0.awaitReplication();
s.rs0.stopPrimary(15);

// Wait for mongos and the config server primary to recognize the new shard primary
awaitRSClientHosts(db.getMongo(), s.rs0.getPrimary(), {ismaster: true});
awaitRSClientHosts(db.getMongo(), s.configRS.getPrimary(), {ismaster: true});

assert.eq(5, db.foo.findOne({_id: 5}).x);
assert.eq(5, db.bar.findOne({_id: 5}).x);

assert.commandWorked(s.s0.adminCommand({shardcollection: "test.bar", key: {_id: 1}}));
s.splitAt("test.bar", {_id: 50});

let yetagain = new Mongo(s.s.name);
assert.eq(5, yetagain.getDB("test").bar.findOne({_id: 5}).x);
assert.eq(5, yetagain.getDB("test").foo.findOne({_id: 5}).x);

assert.eq(5, dbother.bar.findOne({_id: 5}).x);
assert.eq(5, dbother.foo.findOne({_id: 5}).x);

s.stop();
