/**
 * Tests count and distinct using secondaryOk. Also tests a scenario querying a set where only one
 * secondary is up.
 */

// This test shuts down a shard's node and because of this consistency checking
// cannot be performed on that node, which causes the consistency checker to fail.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;
TestData.skipCheckingIndexesConsistentAcrossCluster = true;
TestData.skipCheckOrphans = true;
TestData.skipCheckShardFilteringMetadata = true;

import {awaitRSClientHosts} from "jstests/replsets/rslib.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

let st = new ShardingTest({shards: 1, mongos: 1, other: {rs: true, rs0: {nodes: 2}}});
let rst = st.rs0;

// Insert data into replica set
let conn = new Mongo(st.s.host);

let coll = conn.getCollection("test.countSecondaryOk");
coll.drop();

let bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < 300; i++) {
    bulk.insert({i: i % 10});
}
assert.commandWorked(bulk.execute());

let connA = conn;
let connB = new Mongo(st.s.host);
let connC = new Mongo(st.s.host);

st.printShardingStatus();

// Wait for client to update itself and replication to finish
rst.awaitReplication();

let primary = rst.getPrimary();
let sec = rst.getSecondary();

// Need to check secondaryOk=true first, since secondaryOk=false will destroy conn in pool when
// primary is down.
conn.setSecondaryOk();

// Do a read concern "local" read so that the secondary refreshes its dbVersion before we shut down
// the primary.
coll.runCommand("find", {readConcern: {level: "local"}});

// Data now inserted... stop the primary, since only two in set, other will still be secondary
rst.stop(rst.getPrimary());
printjson(rst.status());

// Wait for the mongos to recognize the secondary
awaitRSClientHosts(conn, sec, {ok: true, secondary: true});

// Make sure that mongos realizes that primary is already down
awaitRSClientHosts(conn, primary, {ok: false});

// count using the command path
assert.eq(30, coll.find({i: 0}).count());
// count using the query path
assert.eq(30, coll.find({i: 0}).itcount());
assert.eq(10, coll.distinct("i").length);

try {
    conn.setSecondaryOk(false);
    // Should throw exception, since not secondaryOk'd
    coll.find({i: 0}).count();

    print("Should not reach here!");
    assert(false);
} catch (e) {
    print("Non-secondaryOk'd connection failed.");
}

// TODO (SERVER-83433): Add back the test coverage for running db hash check on replica
// set that is fsync locked and has replica set endpoint enabled.
st.stop({skipValidation: st.isReplicaSetEndpointActive()});
