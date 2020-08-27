/**
 * Tests count and distinct using secondaryOk. Also tests a scenario querying a set where only one
 * secondary is up.
 */

// This test shuts down a shard's node and because of this consistency checking
// cannot be performed on that node, which causes the consistency checker to fail.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;
TestData.skipCheckingIndexesConsistentAcrossCluster = true;
TestData.skipCheckOrphans = true;

(function() {
'use strict';

load("jstests/replsets/rslib.js");

var st = new ShardingTest({shards: 1, mongos: 1, other: {rs: true, rs0: {nodes: 2}}});
var rst = st.rs0;

// Insert data into replica set
var conn = new Mongo(st.s.host);

var coll = conn.getCollection('test.countSecondaryOk');
coll.drop();

var bulk = coll.initializeUnorderedBulkOp();
for (var i = 0; i < 300; i++) {
    bulk.insert({i: i % 10});
}
assert.commandWorked(bulk.execute());

var connA = conn;
var connB = new Mongo(st.s.host);
var connC = new Mongo(st.s.host);

st.printShardingStatus();

// Wait for client to update itself and replication to finish
rst.awaitReplication();

var primary = rst.getPrimary();
var sec = rst.getSecondary();

// Data now inserted... stop the master, since only two in set, other will still be secondary
rst.stop(rst.getPrimary());
printjson(rst.status());

// Wait for the mongos to recognize the slave
awaitRSClientHosts(conn, sec, {ok: true, secondary: true});

// Make sure that mongos realizes that primary is already down
awaitRSClientHosts(conn, primary, {ok: false});

// Need to check secondaryOk=true first, since secondaryOk=false will destroy conn in pool when
// master is down
conn.setSecondaryOk();

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

st.stop();
})();
