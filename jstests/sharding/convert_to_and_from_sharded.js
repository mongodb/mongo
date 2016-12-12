/**
 * Test that a replica set member can process basic CRUD operations after switching from being
 * a shardsvr and back to non shardsvr.
 * @tags: [requires_persistence]
 */
(function() {

    "use strict";

    var NUM_NODES = 3;

    /**
     * Checks that basic CRUD operations work as expected. Expects the collection to have a
     * { _id: 'marker' } document.
     */
    var checkBasicCRUD = function(coll) {
        var doc = coll.findOne({_id: 'marker', y: {$exists: false}});
        assert.neq(null, doc);

        assert.writeOK(coll.update({_id: 'marker'}, {$set: {y: 2}}));
        assert.eq(2, coll.findOne({_id: 'marker'}).y);

        assert.writeOK(coll.remove({_id: 'marker'}));
        assert.eq(null, coll.findOne({_id: 'marker'}));

        assert.writeOK(coll.insert({_id: 'marker'}, {writeConcern: {w: NUM_NODES}}));
        assert.eq('marker', coll.findOne({_id: 'marker'})._id);
    };

    var st = new ShardingTest({shards: {}});

    var replShard = new ReplSetTest({nodes: NUM_NODES});
    replShard.startSet({verbose: 1});
    replShard.initiate();

    var priConn = replShard.getPrimary();

    assert.writeOK(priConn.getDB('test').unsharded.insert({_id: 'marker'}));
    checkBasicCRUD(priConn.getDB('test').unsharded);

    assert.writeOK(priConn.getDB('test').sharded.insert({_id: 'marker'}));
    checkBasicCRUD(priConn.getDB('test').sharded);

    for (var x = 0; x < NUM_NODES; x++) {
        replShard.restart(x, {shardsvr: ''});
    }

    replShard.awaitNodesAgreeOnPrimary();
    assert.commandWorked(st.s.adminCommand({addShard: replShard.getURL()}));

    priConn = replShard.getPrimary();
    checkBasicCRUD(priConn.getDB('test').unsharded);
    checkBasicCRUD(priConn.getDB('test').sharded);

    checkBasicCRUD(st.s.getDB('test').unsharded);
    checkBasicCRUD(st.s.getDB('test').sharded);

    assert.commandWorked(st.s.adminCommand({enableSharding: 'test'}));
    assert.commandWorked(st.s.adminCommand({shardCollection: 'test.sharded', key: {_id: 1}}));

    checkBasicCRUD(st.s.getDB('test').unsharded);
    checkBasicCRUD(st.s.getDB('test').sharded);

    for (x = 0; x < 4; x++) {
        assert.writeOK(st.s.getDB('test').sharded.insert({_id: x}));
        assert.commandWorked(st.s.adminCommand({split: 'test.sharded', middle: {_id: x}}));
    }

    var newMongod = MongoRunner.runMongod({shardsvr: ''});

    assert.commandWorked(st.s.adminCommand({addShard: newMongod.name, name: 'toRemoveLater'}));

    for (x = 0; x < 2; x++) {
        assert.commandWorked(
            st.s.adminCommand({moveChunk: 'test.sharded', find: {_id: x}, to: 'toRemoveLater'}));
    }

    checkBasicCRUD(st.s.getDB('test').unsharded);
    checkBasicCRUD(st.s.getDB('test').sharded);

    assert.commandWorked(st.s.adminCommand({removeShard: 'toRemoveLater'}));

    // Start the balancer to start draining the chunks.
    st.startBalancer();

    assert.soon(function() {
        var res = st.s.adminCommand({removeShard: 'toRemoveLater'});
        return res.state == 'completed';
    });

    MongoRunner.stopMongod(newMongod.port);

    checkBasicCRUD(st.s.getDB('test').unsharded);
    checkBasicCRUD(st.s.getDB('test').sharded);

    st.stop();

    checkBasicCRUD(priConn.getDB('test').unsharded);
    checkBasicCRUD(priConn.getDB('test').sharded);

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
