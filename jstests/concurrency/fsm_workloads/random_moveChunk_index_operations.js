'use strict';

/**
 * Performs a series of index operations while chunk migrations are running in the background
 * and verifies that indexes are not left in an inconsistent state.
 *
 * @tags: [requires_sharding, assumes_balancer_off, assumes_autosplit_off, requires_fcv_44];
 */

load("jstests/concurrency/fsm_workload_helpers/chunks.js");  // for chunk helpers
load("jstests/sharding/libs/sharded_index_util.js");  // for findInconsistentIndexesAcrossShards

var $config = (function() {
    function threadCollectionName(prefix, tid) {
        return prefix + tid;
    }

    let data = {
        // Specify TTL as a default option during index creation to enable collMod to modify any
        // index.
        expireAfterSeconds: 10000,

        // List of key patterns that are available for index creation.
        availableIndexes: [{a: 1}, {b: 1}, {c: 1}, {d: 1}, {e: 1}],

        // List of index key patterns that we expect to exist for the current thread's collection.
        expectedIndexes: []
    };

    // Create a sharded collection for each thread and split it into two chunks.
    let setup = function setup(db, collName, cluster) {
        for (let tid = 0; tid < this.threadCount; ++tid) {
            // Default collection size.
            const collectionSize = 100;
            const coll = db[threadCollectionName(collName, tid)];
            const fullNs = coll.getFullName();
            assertAlways.commandWorked(db.adminCommand({shardCollection: fullNs, key: {"_id": 1}}));

            // Insert documents
            let bulk = coll.initializeUnorderedBulkOp();
            for (let i = 0; i < collectionSize; i++) {
                bulk.insert({_id: i, a: i, b: i * 2, c: i / 2, d: i * 3, e: i / 3});
            }
            // Execute the inserts and split the data into two chunks.
            assertAlways.commandWorked(bulk.execute());
            const midpoint = collectionSize / 2;
            assertAlways.commandWorked(db.adminCommand({split: fullNs, middle: {_id: midpoint}}));
        }
    };

    let states = {
        init: function(db, collName, connCache) {
            this.collName = threadCollectionName(collName, this.tid);
        },

        moveChunk: function(db, collName, connCache) {
            let tid = this.tid;
            // Pick a tid at random until we pick one that doesn't target this thread's collection.
            while (tid === this.tid)
                tid = Random.randInt(this.threadCount);
            const targetThreadColl = threadCollectionName(collName, tid);

            // Pick a chunk from that thread's collection
            const chunkColl = db.getSiblingDB("config").chunks;
            const targetNs = db.getName() + "." + targetThreadColl;
            const randomChunk =
                chunkColl.aggregate([{$match: {ns: targetNs}}, {$sample: {size: 1}}]).toArray()[0];
            const fromShard = randomChunk.shard;
            const bounds = [randomChunk.min, randomChunk.max];

            // Pick a shard at random to move it to.
            const shardNames = Object.keys(connCache.shards);
            const destinationShards = shardNames.filter(shard => shard !== fromShard);
            const toShard = destinationShards[Random.randInt(destinationShards.length)];

            // Issue a moveChunk command.
            try {
                const waitForDelete = Random.rand() < 0.5;
                ChunkHelper.moveChunk(db, targetThreadColl, bounds, toShard, waitForDelete);
            } catch (e) {
                // Ignore Interrupted errors, which come when a moveChunk is interrupted by a
                // concurrent index operation, and DuplicateKey errors, which come when multiple
                // moveChunks attempt to write to the config.migrations collection at once.
                const acceptableCodes = [ErrorCodes.Interrupted, ErrorCodes.DuplicateKey];
                if (e.code && acceptableCodes.includes(e.code) ||
                    // Indexes may be transiently inconsistent across shards, which can lead a
                    // concurrent migration to abort if the recipient's collection is non-empty.
                    (e.code === ErrorCodes.OperationFailed &&
                     e.message.includes("CannotCreateCollection"))) {
                    print("Ignoring acceptable moveChunk error: " + tojson(e));
                    return;
                }
                throw e;
            }
        },

        // Pick an available keyPattern at random and create an index for it.
        createIndexes: function(db, collName, connCache) {
            if (data.availableIndexes.length === 0) {
                print("Skipping createIndexes; no available key patterns");
                return;
            }
            const idx = Random.randInt(data.availableIndexes.length);
            const index = data.availableIndexes[idx];
            const indexName = Object.keys(index)[0];
            assertAlways.commandWorked(db.runCommand({
                createIndexes: this.collName,
                indexes:
                    [{key: index, name: indexName, expireAfterSeconds: data.expireAfterSeconds}]
            }));

            // Remove created index from available list and record it in expected index map.
            data.availableIndexes.splice(idx, 1);
            data.expectedIndexes.push(index);
        },

        // Pick an existing index at random and drop it.
        dropIndexes: function(db, collName, connCache) {
            if (data.expectedIndexes.length === 0) {
                print("Skipping dropIndexes; no indexes available to drop");
                return;
            }
            const idx = Random.randInt(data.expectedIndexes.length);
            const indexToDrop = data.expectedIndexes[idx];
            const indexName = Object.keys(indexToDrop)[0];
            try {
                assertAlways.commandWorked(
                    db.runCommand({dropIndexes: this.collName, index: indexName}));
            } catch (e) {
                // Since dropping an index across shards is not atomic, it can be the case that
                // the cluster ends up in an inconsistent state temporarily. In particular, a
                // successful dropIndexes may retry on a stale config error only for the retry
                // to see that the index was dropped by the first attempt. Therefore, we ignore this
                // error.
                if (e.code === ErrorCodes.IndexNotFound) {
                    print("Ignoring acceptable dropIndexes error: " + tojson(e));
                } else {
                    throw e;
                }
            }

            // Remove dropped index from list of existing indexes and add key pattern back to
            // available list.
            data.expectedIndexes.splice(idx, 1);
            data.availableIndexes.push(indexToDrop);
        },

        collMod: function(db, collName, connCache) {
            if (data.expectedIndexes.length === 0) {
                print("Skipping collMod; no indexes available to modify");
                return;
            }
            const idx = Random.randInt(data.expectedIndexes.length);
            const indexToModify = data.expectedIndexes[idx];
            data.expireAfterSeconds++;
            const result = db.runCommand({
                collMod: this.collName,
                index: {keyPattern: indexToModify, expireAfterSeconds: data.expireAfterSeconds}
            });
            assertAlways.commandWorked(result);
        },

        // Verify that the indexes that we expect to be on disk are actually there and that indexes
        // are consistent across all shards for this thread's collection.
        //
        // Because it is possible that indexes can be temporarily inconsistent, we retry this
        // verification up to 3 times, after which point it's very unlikely that inconsistent
        // indexes have persisted across shards.
        //
        // Note that we retry after waiting for 2 seconds to allow for any temporarily
        // inconsistent indexes the chance to clear up.
        verifyIndexes: function(db, collName, connCache) {
            function getKeyPattern(index) {
                assertAlways.hasFields(index, ["spec"]);
                const spec = index["spec"];
                assertAlways.hasFields(spec, ["key"]);
                return spec["key"];
            }

            function checkConsistentIndexes(collName, data) {
                // Check that the indexes that we expect to exist actually do.
                const actualIndexes = ShardedIndexUtil.getPerShardIndexes(db[collName]);
                for (let expectedIndex of data.expectedIndexes) {
                    let match = actualIndexes.some((indexList) => {
                        assertAlways.hasFields(indexList, ["indexes"]);
                        const indexes = indexList["indexes"];
                        const indexKeyPatterns = indexes.map(index => getKeyPattern(index));
                        return ShardedIndexUtil.containsBSON(indexKeyPatterns, expectedIndex);
                    });
                    if (!match) {
                        print(`Could not find index ${
                            tojson(expectedIndex)} on any shard. Indexes found: ${
                            tojson(actualIndexes)}`);
                        return false;
                    }
                }

                // Check that each shard has each reported index.
                const inconsistentIndexes =
                    ShardedIndexUtil.findInconsistentIndexesAcrossShards(actualIndexes, false);
                for (const shard in inconsistentIndexes) {
                    const shardInconsistentIndexes = inconsistentIndexes[shard];
                    if (shardInconsistentIndexes.length !== 0) {
                        print(`found inconsistent indexes for ${thread.collName}: ${
                            tojson(inconsistentIndexes)}`);
                        return false;
                    }
                }
                return true;
            }

            assert.retry(() => checkConsistentIndexes(this.collName, data),
                         `Detected inconsistent indexes `,
                         3,
                         2 * 1000,
                         {runHangAnalyzer: false});
        }
    };

    let transitions = {
        // First step should always be to create an index so that we can have at least one index to
        // drop or modify when transitioning to either dropIndexes or collMod.
        init: {createIndexes: 1.0},
        createIndexes:
            {moveChunk: .4, createIndexes: .15, dropIndexes: .15, collMod: .15, verifyIndexes: .15},
        moveChunk: {createIndexes: .25, dropIndexes: .25, collMod: .25, verifyIndexes: .25},
        dropIndexes:
            {moveChunk: .4, createIndexes: .15, dropIndexes: .15, collMod: .15, verifyIndexes: .15},
        collMod:
            {moveChunk: .4, createIndexes: .15, dropIndexes: .15, collMod: .15, verifyIndexes: .15},
        verifyIndexes: {moveChunk: .25, createIndexes: .25, dropIndexes: .25, collMod: .25},
    };

    return {
        threadCount: 5,
        iterations: 50,
        startState: 'init',
        states: states,
        transitions: transitions,
        data: data,
        setup: setup,
        passConnectionCache: true
    };
})();
