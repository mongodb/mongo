'use strict';

/**
 * Performs a series of placement-changing commands (DDLs and chunk migrations) while
 * resetPlacementHistory may be run in parallel. After tearing down the test, the
 * check_routing_table_consistency hook will verify that the content config.placementHistory will
 * still be consistent with the rest of the catalog.
 *
 * @tags: [
 *   requires_fcv_71,
 *   requires_sharding,
 *   assumes_balancer_off,
 *   does_not_support_causal_consistency,
 *   does_not_support_add_remove_shards,
 *   # The mechanism to pick a random collection is not resilient to stepdowns
 *   does_not_support_stepdowns,
 *   does_not_support_transactions,
 *  ]
 */

load("jstests/libs/uuid_util.js");
load('jstests/libs/feature_flag_util.js');
load('jstests/concurrency/fsm_workload_helpers/chunks.js');

var $config = (function() {
    const testCollectionsState = 'testCollectionsState';
    const resetPlacementHistoryState = 'resetPlacementHistoryState';
    const resetPlacementHistoryStateId = 'x';
    const numThreads = 12;
    const numTestCollections = numThreads + 5;

    function getConfig(db) {
        return db.getSiblingDB('config');
    }

    /**
     * Used to guarantee that a namespace isn't targeted by multiple FSM thread at the same time.
     */
    function acquireCollectionName(db, mustBeAlreadyCreated = true) {
        let acquiredCollDoc = null;
        assertAlways.soon(function() {
            const query = {acquired: false};
            if (mustBeAlreadyCreated) {
                query.created = true;
            }
            acquiredCollDoc = db[testCollectionsState].findAndModify({
                query: query,
                sort: {lastAcquired: 1},
                update: {$set: {acquired: true, lastAcquired: new Date()}}
            });
            return acquiredCollDoc !== null;
        });
        return acquiredCollDoc.collName;
    }

    function releaseCollectionName(db, collName, wasDropped = false) {
        // in case of collection dropped, leave a chance of reusing the same name during the next
        // shardCollection
        const newExtension = wasDropped && Math.random() < 0.5 ? 'e' : '';
        const match = db[testCollectionsState].findAndModify({
            query: {collName: collName, acquired: true},
            update:
                {$set: {collName: collName + newExtension, acquired: false, created: !wasDropped}}
        });
        assertAlways(match !== null);
    }

    let states = {
        shardCollection: function(db, _, connCache) {
            // To avoid starvation problems during the execution of the FSM, it is OK to pick
            // up an already sharded collection.
            const collName = acquireCollectionName(db, false /*mustBeAlreadyCreated*/);
            try {
                jsTestLog(`Beginning shardCollection state for ${collName}`);
                assertAlways.commandWorked(
                    db.adminCommand({shardCollection: db[collName].getFullName(), key: {_id: 1}}));
                jsTestLog(`shardCollection state for ${collName} completed`);
            } catch (e) {
                throw e;
            } finally {
                releaseCollectionName(db, collName);
            }
        },

        dropCollection: function(db, _, connCache) {
            // To avoid starvation problems during the execution of the FSM, it is OK to pick
            // up an already dropped collection.
            const collName = acquireCollectionName(db, false /*mustBeAlreadyCreated*/);
            try {
                jsTestLog(`Beginning dropCollection state for ${collName}`);
                // Avoid checking the outcome, as the drop may result into a no-op.
                db[collName].drop();
                jsTestLog(`dropCollection state for ${collName} completed`);
            } catch (e) {
                throw e;
            } finally {
                releaseCollectionName(db, collName, true /*wasDropped*/);
            }
        },

        renameCollection: function(db, _, connCache) {
            const collName = acquireCollectionName(db);
            const renamedCollName = collName + '_renamed';
            try {
                jsTestLog(`Beginning renameCollection state for ${collName}`);
                assertAlways.commandWorked(db[collName].renameCollection(renamedCollName));
                // reverse the rename before leaving the state.
                assertAlways.commandWorked(db[renamedCollName].renameCollection(collName));
                jsTestLog(`renameCollection state for ${collName} completed`);
            } catch (e) {
                throw e;
            } finally {
                releaseCollectionName(db, collName);
            }
        },

        moveChunk: function(db, _, connCache) {
            const collName = acquireCollectionName(db);
            try {
                jsTestLog(`Beginning moveChunk state for ${collName}`);
                const collUUID =
                    getConfig(db).collections.findOne({_id: db[collName].getFullName()}).uuid;
                assertAlways(collUUID);
                const shards = getConfig(db).shards.find().toArray();
                const chunkToMove = getConfig(db).chunks.findOne({uuid: collUUID});
                const destination = shards.filter(
                    s => s._id !==
                        chunkToMove.shard)[Math.floor(Math.random() * (shards.length - 1))];
                ChunkHelper.moveChunk(
                    db, collName, [chunkToMove.min, chunkToMove.max], destination._id, true);
                jsTestLog(`moveChunk state for ${collName} completed`);
            } catch (e) {
                throw e;
            } finally {
                releaseCollectionName(db, collName);
            }
        },

        resetPlacementHistory: function(db, collName, connCache) {
            jsTestLog(`Beginning resetPlacementHistory state`);
            assertAlways.commandWorked(db.adminCommand({resetPlacementHistory: 1}));
            jsTestLog(`resetPlacementHistory state completed`);
        },

    };

    let transitions = {
        shardCollection: {
            shardCollection: 0.22,
            dropCollection: 0.22,
            renameCollection: 0.22,
            moveChunk: 0.22,
            resetPlacementHistory: 0.12
        },
        dropCollection: {
            shardCollection: 0.22,
            dropCollection: 0.22,
            renameCollection: 0.22,
            moveChunk: 0.22,
            resetPlacementHistory: 0.12
        },
        renameCollection: {
            shardCollection: 0.22,
            dropCollection: 0.22,
            renameCollection: 0.22,
            moveChunk: 0.22,
            resetPlacementHistory: 0.12
        },
        moveChunk: {
            shardCollection: 0.22,
            dropCollection: 0.22,
            renameCollection: 0.22,
            moveChunk: 0.22,
            resetPlacementHistory: 0.12
        },
        resetPlacementHistory: {
            shardCollection: 0.22,
            dropCollection: 0.22,
            renameCollection: 0.22,
            moveChunk: 0.22,
        },
    };

    let setup = function(db, _, cluster) {
        this.skipMetadataChecks =
            // TODO SERVER-70396: remove this flag
            !FeatureFlagUtil.isEnabled(db.getMongo(), 'CheckMetadataConsistency');

        for (let i = 0; i < numTestCollections; ++i) {
            db[testCollectionsState].insert({
                collName: `testColl_${i}`,
                acquired: false,
                lastAcquired: new Date(),
                created: false
            });
        }

        db[resetPlacementHistoryState].insert({_id: resetPlacementHistoryStateId, ongoing: false});
    };

    let teardown = function(db, collName, cluster) {};

    return {
        threadCount: numThreads,
        iterations: 32,
        startState: 'shardCollection',
        states: states,
        transitions: transitions,
        setup: setup,
        teardown: teardown,
        passConnectionCache: true
    };
})();
