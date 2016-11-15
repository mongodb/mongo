'use strict';

/**
 * sharded_moveChunk_drop_shard_key_index.js
 *
 * Tests that dropping the shard key index while migrating a chunk doesn't cause the shard to abort.
 *
 * This workload was designed to reproduce SERVER-24994.
 */

var $config = (function() {

    var data = {numSplitPoints: 100, shardKey: {key: 1}};

    var states = {

        init: function init(db, collName) {
            // No-op
        },

        moveChunk: function moveChunk(db, collName) {
            var configDB = db.getSiblingDB('config');
            var shards = configDB.shards.aggregate([{$sample: {size: 1}}]).toArray();
            assertAlways.eq(1, shards.length, tojson(shards));

            var shardName = shards[0]._id;
            var chunkBoundary = Random.randInt(this.numSplitPoints);

            // We don't assert that the command succeeded when migrating a chunk because it's
            // possible another thread has already started migrating a chunk.
            db.adminCommand({
                moveChunk: db[collName].getFullName(),
                find: {key: chunkBoundary},
                to: shardName,
                _waitForDelete: true,
            });
        },

        dropIndex: function dropIndex(db, collName) {
            // We don't assert that the command succeeded when dropping an index because it's
            // possible another thread has already dropped this index.
            db[collName].dropIndex(this.shardKey);

            // Re-create the index that was dropped.
            assertAlways.commandWorked(db[collName].createIndex(this.shardKey));
        }

    };

    var transitions = {
        init: {moveChunk: 0.5, dropIndex: 0.5},
        moveChunk: {moveChunk: 0.5, dropIndex: 0.5},
        dropIndex: {moveChunk: 0.5, dropIndex: 0.5}
    };

    function setup(db, collName, cluster) {
        var bulk = db[collName].initializeUnorderedBulkOp();
        for (var i = 0; i < this.numSplitPoints; ++i) {
            bulk.insert({key: i});
        }

        var res = bulk.execute();
        assertAlways.writeOK(res);
        assertAlways.eq(this.numSplitPoints, res.nInserted, tojson(res));

        for (i = 0; i < this.numSplitPoints; ++i) {
            assertWhenOwnColl.commandWorked(
                db.adminCommand({split: db[collName].getFullName(), middle: {key: i}}));
        }
    }

    return {
        threadCount: 10,
        iterations: 100,
        data: data,
        states: states,
        transitions: transitions,
        setup: setup
    };

})();
