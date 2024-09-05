'use strict';

/**
 * Test shardCollection and reshardCollection commands against a collection without a supporting
 * index for the shard key do not implicitly create the shard key index if it is run with
 * "implicitlyCreateIndex" false.
 *
 * @tags: [
 *   requires_sharding,
 *   requires_fcv_81,
 *   featureFlagHashedShardKeyIndexOptionalUponShardingCollection,
 *   # This workload cannot run in a suite that removes shards since the test collection in this
 *   # workload is sharded but does not have a shard key index so the balancer cannot move it out of
 *   # the shard being removed (using moveChunk).
 *   assumes_stable_shard_list
 * ];
 */

function assertIndexDoesNotExist(db, collName, indexKey) {
    const indexes = db.getCollection(collName).getIndexes();
    assert(indexes.every(index => bsonWoCompare(index.key, indexKey) != 0), indexes);
}

/**
 * Returns a random integer between 'min' and 'max' (not inclusive).
 */
function getRandInteger(min, max) {
    return Math.floor(Math.random() * (max - min)) + min;
}

export const $config = (function() {
    // Prevent the FSM runner from sharding the test collection so the collection can be sharded
    // with "implicitlyCreateIndex" false during the setup phase below. The original
    // 'shardCollectionProbability' is restored in the teardown phase.
    var originalShardCollectionProbability = TestData.shardCollectionProbability;
    TestData.shardCollectionProbability = 0;

    var data = {
        originalShardCollectionProbability,
        numDocs: 1000,
        oldShardKey: {key0: "hashed"},
        newShardKey: {key1: "hashed"},
    };

    var setup = function(db, collName, cluster) {
        assert.commandWorked(db.adminCommand({
            shardCollection: `${db}.${collName}`,
            key: this.oldShardKey,
            implicitlyCreateIndex: false
        }));
        const docs = [];
        for (let i = 0; i < this.numDocs; i++) {
            docs.push({key0: i, key1: i, key2: i});
        }
        assert.commandWorked(db.runCommand({insert: collName, documents: docs}));
    };

    var teardown = function(db, collName, cluster) {
        TestData.shardCollectionProbability = this.originalShardCollectionProbability;
    };

    var states = (function() {
        function init(db, collName, connCache) {
        }

        function reshardCollection(db, collName, connCache) {
            assert.commandWorked(db.adminCommand({
                reshardCollection: `${db}.${collName}`,
                key: this.newShardKey,
                forceRedistribution: true,
                implicitlyCreateIndex: false
            }));
        }

        function verifyIndexes(db, collName, connCache) {
            assertIndexDoesNotExist(db, collName, this.oldShardKey);
            assertIndexDoesNotExist(db, collName, this.newShardKey);
        }

        function find(db, collName, connCache) {
            const value = getRandInteger(0, this.numDocs);
            const docs = db.getCollection(collName).find({key0: value, key1: value}).toArray();
            assert.eq(docs.length, 1, docs);
        }

        function update(db, collName, connCache) {
            const value = getRandInteger(0, this.numDocs);
            const res = assert.commandWorked(
                db.getCollection(collName).update({key0: value, key1: value}, {$inc: {key2: 1}}));
            assert.eq(res.nMatched, 1, res);
            assert.eq(res.nModified, 1, res);
        }

        return {init, reshardCollection, verifyIndexes, find, update};
    })();

    var weights = {
        reshardCollection: 0.3,
        verifyIndexes: 0.3,
        find: 0.2,
        update: 0.2,
    };

    var transitions = {
        init: weights,
        reshardCollection: weights,
        verifyIndexes: weights,
        find: weights,
        update: weights,
    };

    return {
        threadCount: 5,
        iterations: 20,
        setup: setup,
        teardown: teardown,
        startState: "init",
        states: states,
        transitions: transitions,
        data: data,
        passConnectionCache: true
    };
})();
