'use strict';

/**
 * Performs a series of CRUD operations while DDL commands are running in the background
 * and verifies guarantees are not broken.
 *
 * @tags: [
 *   requires_sharding,
 *   assumes_balancer_off,
 *   does_not_support_causal_consistency,
 *   # TODO (SERVER-56879): Support add/remove shards in new DDL paths
 *   does_not_support_add_remove_shards,
 *   # The mutex mechanism used in CRUD and drop states does not support stepdown
 *   does_not_support_stepdowns,
 *   # Can be removed once PM-1965-Milestone-1 is completed.
 *   does_not_support_transactions,
 *  ]
 */

load("jstests/concurrency/fsm_workload_helpers/state_transition_utils.js");
load("jstests/libs/uuid_util.js");
load('jstests/libs/feature_flag_util.js');

var $config = (function() {
    function threadCollectionName(prefix, tid) {
        return prefix + tid;
    }

    function countDocuments(coll, query) {
        var count;
        assert.soon(() => {
            try {
                count = coll.countDocuments(query);
                return true;
            } catch (e) {
                if (e.code === ErrorCodes.QueryPlanKilled) {
                    // Retry. Can happen due to concurrent rename collection.
                    return false;
                }
                throw e;
            }
        });

        return count;
    }

    let data = {numChunks: 20, documentsPerChunk: 5, CRUDMutex: 'CRUDMutex'};

    /**
     * Used for mutual exclusion. Uses a collection to ensure atomicity on the read and update
     * operation.
     */
    function mutexLock(db, tid, collName) {
        jsTestLog('Trying to acquire mutexLock for resource tid:' + tid +
                  ' collection:' + collName);
        assertAlways.soon(() => {
            let doc =
                db[data.CRUDMutex].findAndModify({query: {tid: tid}, update: {$set: {mutex: 1}}});
            return doc.mutex === 0;
        });
        jsTestLog('Acquired mutexLock for tid:' + tid + ' collection:' + collName);
    }

    function mutexUnlock(db, tid, collName) {
        db[data.CRUDMutex].update({tid: tid}, {$set: {mutex: 0}});
        jsTestLog('Unlocked lock for resource tid:' + tid + ' collection:' + collName);
    }

    let states = {
        init: function(db, collName, connCache) {
            this.collName = threadCollectionName(collName, this.tid);
        },

        create: function(db, collName, connCache) {
            let tid = this.tid;
            // Pick a tid at random until we pick one that doesn't target this thread's collection.
            while (tid === this.tid)
                tid = Random.randInt(this.threadCount);

            const targetThreadColl = threadCollectionName(collName, tid);
            const coll = db[threadCollectionName(collName, tid)];
            const fullNs = coll.getFullName();
            jsTestLog('create state tid:' + tid + ' currentTid:' + this.tid +
                      ' collection:' + targetThreadColl);
            assertAlways.commandWorked(
                db.adminCommand({shardCollection: fullNs, key: {_id: 1}, unique: false}));
            jsTestLog('create state finished');
        },
        drop: function(db, collName, connCache) {
            let tid = this.tid;
            // Pick a tid at random until we pick one that doesn't target this thread's collection.
            while (tid === this.tid)
                tid = Random.randInt(this.threadCount);

            const targetThreadColl = threadCollectionName(collName, tid);

            jsTestLog('drop state tid:' + tid + ' currentTid:' + this.tid +
                      ' collection:' + targetThreadColl);
            mutexLock(db, tid, targetThreadColl);
            try {
                assertAlways.eq(db[targetThreadColl].drop(), true);
            } finally {
                mutexUnlock(db, tid, targetThreadColl);
            }
            jsTestLog('drop state finished');
        },
        rename: function(db, collName, connCache) {
            let tid = this.tid;
            // Pick a tid at random until we pick one that doesn't target this thread's collection.
            while (tid === this.tid)
                tid = Random.randInt(this.threadCount);
            const srcCollName = threadCollectionName(collName, tid);
            const srcColl = db[srcCollName];
            // Rename collection
            const destCollName =
                threadCollectionName(collName, tid + '_' + extractUUIDFromObject(UUID()));
            try {
                jsTestLog('rename state tid:' + tid + ' currentTid:' + this.tid +
                          ' collection:' + srcCollName + ' dst:' + destCollName);
                assertAlways.commandWorked(srcColl.renameCollection(destCollName));
            } catch (e) {
                const exceptionCode = e.code;
                if (exceptionCode) {
                    if (exceptionCode === ErrorCodes.NamespaceNotFound) {
                        // It is fine for a rename operation to throw NamespaceNotFound BEFORE
                        // starting (e.g. if the collection was previously dropped). Checking the
                        // changelog to assert that no such exception was thrown AFTER a rename
                        // started.
                        const dbName = db.getName();
                        let config = db.getSiblingDB('config');
                        let countRenames = config.changelog
                                               .find({
                                                   what: 'renameCollection.start',
                                                   details: {
                                                       source: dbName + srcCollName,
                                                       destination: dbName + destCollName
                                                   }
                                               })
                                               .itcount();
                        assert.eq(0,
                                  countRenames,
                                  'NamespaceNotFound exception thrown during rename from ' +
                                      srcCollName + ' to ' + destCollName);
                        return;
                    }
                    if (exceptionCode === ErrorCodes.ConflictingOperationInProgress) {
                        // It is fine for a rename operation to throw ConflictingOperationInProgress
                        // if a concurrent rename with the same source collection but different
                        // options is ongoing.
                        return;
                    }
                }
                throw e;
            } finally {
                jsTestLog('rename state finished');
            }
        },
        checkDatabaseMetadataConsistency: function(db, collName, connCache) {
            if (this.skipMetadataChecks) {
                return;
            }
            jsTestLog('Check database metadata state');
            const inconsistencies = db.checkMetadataConsistency().toArray();
            assertAlways.eq(0, inconsistencies.length, tojson(inconsistencies));
        },
        checkCollectionMetadataConsistency: function(db, collName, connCache) {
            if (this.skipMetadataChecks) {
                return;
            }

            let tid = this.tid;
            while (tid === this.tid)
                tid = Random.randInt(this.threadCount);

            const targetThreadColl = threadCollectionName(collName, tid);
            jsTestLog('Check collection metadata state tid:' + tid + ' currentTid:' + this.tid +
                      ' collection:' + targetThreadColl);
            const inconsistencies = db[targetThreadColl].checkMetadataConsistency().toArray();
            assertAlways.eq(0, inconsistencies.length, tojson(inconsistencies));
        },
        CRUD: function(db, collName, connCache) {
            let tid = this.tid;
            // Pick a tid at random until we pick one that doesn't target this thread's collection.
            while (tid === this.tid)
                tid = Random.randInt(this.threadCount);

            const targetThreadColl = threadCollectionName(collName, tid);
            jsTestLog('CRUD state tid:' + tid + ' currentTid:' + this.tid +
                      ' collection:' + targetThreadColl);
            const coll = db[targetThreadColl];
            const fullNs = coll.getFullName();

            const generation = new Date().getTime();
            // Insert Data
            const numDocs = data.documentsPerChunk * data.numChunks;

            let insertBulkOp = coll.initializeUnorderedBulkOp();
            for (let i = 0; i < numDocs; ++i) {
                insertBulkOp.insert({generation: generation, count: i, tid: tid});
            }

            mutexLock(db, tid, targetThreadColl);
            try {
                jsTestLog('CRUD - Insert tid:' + tid + ' currentTid:' + this.tid +
                          ' collection:' + targetThreadColl);
                // Check if insert succeeded
                var res = insertBulkOp.execute();
                assertAlways.commandWorked(res);

                let currentDocs = countDocuments(coll, {generation: generation});

                // Check guarantees IF NO CONCURRENT DROP is running.
                // If a concurrent rename came in, then either the full operation succeded (meaning
                // there will be 0 documents left) or the insert came in first.
                assertAlways(currentDocs === numDocs || currentDocs === 0);

                jsTestLog('CRUD - Update tid:' + tid + ' currentTid:' + this.tid +
                          ' collection:' + targetThreadColl);
                var res =
                    coll.update({generation: generation}, {$set: {updated: true}}, {multi: true});
                if (res.hasWriteError()) {
                    var err = res.getWriteError();
                    if (err.code == ErrorCodes.QueryPlanKilled) {
                        // Update is expected to throw ErrorCodes::QueryPlanKilled if performed
                        // concurrently with a rename (SERVER-31695).
                        jsTestLog('CRUD state finished earlier because query plan was killed.');
                        return;
                    }
                    throw e;
                }
                assertAlways.commandWorked(res);

                // Delete Data
                jsTestLog('CRUD - Remove tid:' + tid + ' currentTid:' + this.tid +
                          ' collection:' + targetThreadColl);
                // Check if delete succeeded
                coll.remove({generation: generation}, {multi: true});
                // Check guarantees IF NO CONCURRENT DROP is running.
                assertAlways.eq(countDocuments(coll, {generation: generation}), 0);
            } finally {
                mutexUnlock(db, tid, targetThreadColl);
            }
            jsTestLog('CRUD state finished');
        }
    };

    let setup = function(db, collName, cluster) {
        this.skipMetadataChecks =
            // TODO SERVER-70396: remove this flag
            !FeatureFlagUtil.isEnabled(db.getMongo(), 'CheckMetadataConsistency');

        for (let tid = 0; tid < this.threadCount; ++tid) {
            db[data.CRUDMutex].insert({tid: tid, mutex: 0});
        }
    };

    let teardown = function(db, collName, cluster) {};

    return {
        threadCount: 12,
        iterations: 64,
        startState: 'init',
        states: states,
        transitions: uniformDistTransitions(states),
        data: data,
        setup: setup,
        teardown: teardown,
        passConnectionCache: true
    };
})();
