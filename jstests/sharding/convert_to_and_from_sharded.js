/**
 * Test that a replica set member can process basic CRUD operations after switching from being
 * a shardsvr and back to non shardsvr.
 * @tags: [requires_persistence]
 */
(function() {
"use strict";
load('jstests/sharding/libs/remove_shard_util.js');

// TODO SERVER-50144 Remove this and allow orphan checking.
// This test calls removeShard which can leave docs in config.rangeDeletions in state "pending",
// therefore preventing orphans from being cleaned up.
TestData.skipCheckOrphans = true;

var NUM_NODES = 3;

/**
 * Checks that basic CRUD operations work as expected. Expects the collection to have a
 * { _id: 'marker' } document.
 */
var checkBasicCRUD = function(coll) {
    var doc = coll.findOne({_id: 'marker', y: {$exists: false}});
    assert.neq(null, doc);

    assert.commandWorked(coll.update({_id: 'marker'}, {$set: {y: 2}}));
    assert.eq(2, coll.findOne({_id: 'marker'}).y);

    assert.commandWorked(coll.remove({_id: 'marker'}));
    assert.eq(null, coll.findOne({_id: 'marker'}));

    assert.commandWorked(coll.insert({_id: 'marker'}, {writeConcern: {w: NUM_NODES}}));
    assert.eq('marker', coll.findOne({_id: 'marker'})._id);
};

const numShards = TestData.configShard ? 1 : 0;
var st = new ShardingTest({shards: numShards});

var replShard = new ReplSetTest({nodes: NUM_NODES});
replShard.startSet({verbose: 1});
replShard.initiate();

var priConn = replShard.getPrimary();

// Starting a brand new replica set without '--shardsvr' will cause the FCV to be written as the
// latest available for that binary. This poses a problem when this test is run in the mixed
// version suite because mongos will be 'last-lts' and if this node is of the latest binary,
// it will report itself as the 'latest' FCV, which would cause mongos to refuse to connect to
// it and shutdown.
//
// In order to work around this, in the mixed version suite, be pessimistic and always set this
// node to the 'last-lts' FCV
if (jsTestOptions().shardMixedBinVersions) {
    assert.commandWorked(priConn.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
    replShard.awaitReplication();
}

assert.commandWorked(priConn.getDB('test').unsharded.insert({_id: 'marker'}));
checkBasicCRUD(priConn.getDB('test').unsharded);

assert.commandWorked(priConn.getDB('test').sharded.insert({_id: 'marker'}));
checkBasicCRUD(priConn.getDB('test').sharded);

for (var x = 0; x < NUM_NODES; x++) {
    replShard.restart(x, {
        shardsvr: '',
    });
}

replShard.awaitNodesAgreeOnPrimary();
assert.commandWorked(st.s.adminCommand({addShard: replShard.getURL()}));

checkBasicCRUD(st.s.getDB('test').unsharded);
checkBasicCRUD(st.s.getDB('test').sharded);

assert.commandWorked(st.s.adminCommand({enableSharding: 'test'}));
assert.commandWorked(st.s.adminCommand({shardCollection: 'test.sharded', key: {_id: 1}}));

checkBasicCRUD(st.s.getDB('test').unsharded);
checkBasicCRUD(st.s.getDB('test').sharded);

for (x = 0; x < 4; x++) {
    assert.commandWorked(st.s.getDB('test').sharded.insert({_id: x}));
    assert.commandWorked(st.s.adminCommand({split: 'test.sharded', middle: {_id: x}}));
}

let newShard =
    new ReplSetTest({name: "toRemoveLater", nodes: NUM_NODES, nodeOptions: {shardsvr: ""}});
newShard.startSet();
newShard.initiate();

assert.commandWorked(st.s.adminCommand({addShard: newShard.getURL(), name: 'toRemoveLater'}));

for (x = 0; x < 2; x++) {
    assert.commandWorked(
        st.s.adminCommand({moveChunk: 'test.sharded', find: {_id: x}, to: 'toRemoveLater'}));
}

checkBasicCRUD(st.s.getDB('test').unsharded);
checkBasicCRUD(st.s.getDB('test').sharded);

// Start the balancer to start draining the chunks.
st.startBalancer();

removeShard(st, 'toRemoveLater');

newShard.stopSet();

checkBasicCRUD(st.s.getDB('test').unsharded);
checkBasicCRUD(st.s.getDB('test').sharded);

st.stop();

jsTest.log('About to restart repl w/o shardsvr');

replShard.nodes.forEach(function(node) {
    delete node.fullOptions.shardsvr;
});

replShard.restart(replShard.nodes);
replShard.awaitNodesAgreeOnPrimary();

priConn = replShard.getPrimary();
checkBasicCRUD(priConn.getDB('test').unsharded);
checkBasicCRUD(priConn.getDB('test').sharded);
replShard.stopSet();
})();
