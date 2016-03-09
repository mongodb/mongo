// Tests of sharded GLE enforcing write concern against operations in a cluster
// Basic sharded GLE operation is tested elsewhere.
//
// This test asserts that a journaled write to a mongod running with --nojournal should be rejected,
// so cannot be run on the ephemeralForTest storage engine, as it accepts all journaled writes.
// @tags: [SERVER-21420]

(function() {
    'use strict';

    // Options for a cluster with two replica set shards, the first with two nodes the second with
    // one
    // This lets us try a number of GLE scenarios
    var options = {
        rs: true,
        rsOptions: {nojournal: ""},
        // Options for each replica set shard
        rs0: {nodes: 3},
        rs1: {nodes: 3}
    };

    var st = new ShardingTest({shards: 2, other: options});

    var mongos = st.s0;
    var admin = mongos.getDB("admin");
    var config = mongos.getDB("config");
    var coll = mongos.getCollection(jsTestName() + ".coll");
    var shards = config.shards.find().toArray();

    assert.commandWorked(admin.runCommand({enableSharding: coll.getDB().toString()}));
    printjson(admin.runCommand({movePrimary: coll.getDB().toString(), to: shards[0]._id}));
    assert.commandWorked(admin.runCommand({shardCollection: coll.toString(), key: {_id: 1}}));
    assert.commandWorked(admin.runCommand({split: coll.toString(), middle: {_id: 0}}));
    assert.commandWorked(
        admin.runCommand({moveChunk: coll.toString(), find: {_id: 0}, to: shards[1]._id}));

    st.printShardingStatus();

    var gle = null;

    //
    // No journal insert, GLE fails
    coll.remove({});
    coll.insert({_id: 1});
    printjson(gle = coll.getDB().runCommand({getLastError: 1, j: true}));
    assert(!gle.ok);
    assert(gle.errmsg);

    //
    // Successful insert, write concern mode invalid
    coll.remove({});
    coll.insert({_id: -1});
    printjson(gle = coll.getDB().runCommand({getLastError: 1, w: 'invalid'}));
    assert(!gle.ok);
    assert(!gle.err);
    assert(gle.errmsg);
    assert.eq(gle.code, 79);  // UnknownReplWriteConcern - needed for backwards compatibility
    assert.eq(coll.count(), 1);

    //
    // Error on insert (dup key), write concern error not reported
    coll.remove({});
    coll.insert({_id: -1});
    coll.insert({_id: -1});
    printjson(gle = coll.getDB().runCommand({getLastError: 1, w: 'invalid'}));
    assert(gle.ok);
    assert(gle.err);
    assert(gle.code);
    assert(!gle.errmsg);
    assert.eq(coll.count(), 1);

    //
    // Successful remove on one shard, write concern timeout on the other
    var s0Id = st.rs0.getNodeId(st.rs0.liveNodes.slaves[0]);
    st.rs0.stop(s0Id);
    coll.remove({});
    st.rs1.awaitReplication();  // To ensure the first shard won't timeout
    printjson(gle = coll.getDB().runCommand({getLastError: 1, w: 3, wtimeout: 5 * 1000}));
    assert(gle.ok);
    assert.eq(gle.err, 'timeout');
    assert(gle.wtimeout);
    assert(gle.shards);
    assert.eq(coll.count(), 0);

    //
    // Successful remove on two hosts, write concern timeout on both
    // We don't aggregate two timeouts together
    var s1Id = st.rs1.getNodeId(st.rs1.liveNodes.slaves[0]);
    st.rs1.stop(s1Id);
    // new writes to both shards to ensure that remove will do something on both of them
    coll.insert({_id: -1});
    coll.insert({_id: 1});

    coll.remove({});
    printjson(gle = coll.getDB().runCommand({getLastError: 1, w: 3, wtimeout: 5 * 1000}));

    assert(!gle.ok);
    assert(gle.errmsg);
    assert.eq(gle.code, 64);  // WriteConcernFailed - needed for backwards compatibility
    assert(!gle.wtimeout);
    assert(gle.shards);
    assert(gle.errs);
    assert.eq(coll.count(), 0);

    //
    // First replica set with no primary
    //

    //
    // Successful bulk insert on two hosts, host changes before gle (error contacting host)
    coll.remove({});
    coll.insert([{_id: 1}, {_id: -1}]);
    // Wait for write to be written to shards before shutting it down.
    printjson(gle = coll.getDB().runCommand({getLastError: 1}));
    st.rs0.stop(st.rs0.getPrimary(), true);  // wait for stop
    printjson(gle = coll.getDB().runCommand({getLastError: 1}));
    // Should get an error about contacting dead host.
    assert(!gle.ok);
    assert(gle.errmsg);
    assert.eq(coll.count({_id: 1}), 1);

    //
    // Failed insert on two hosts, first replica set with no primary
    // NOTE: This is DIFFERENT from 2.4, since we don't need to contact a host we didn't get
    // successful writes from.
    coll.remove({_id: 1});
    coll.insert([{_id: 1}, {_id: -1}]);

    printjson(gle = coll.getDB().runCommand({getLastError: 1}));
    assert(gle.ok);
    assert(gle.err);
    assert.eq(coll.count({_id: 1}), 1);

    st.stop();

})();
