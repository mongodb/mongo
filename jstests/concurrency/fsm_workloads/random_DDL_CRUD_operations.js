/**
 * Performs a series of CRUD operations while DDL commands are running in the background
 * and verifies guarantees are not broken.
 *
 * @tags: [
 *   requires_sharding,
 *   assumes_balancer_off,
 *   does_not_support_causal_consistency,
 *   # The mutex mechanism used in CRUD and drop states does not support stepdown
 *   does_not_support_stepdowns,
 *   # Can be removed once PM-1965-Milestone-1 is completed.
 *   does_not_support_transactions,
 *  ]
 */

import {
    uniformDistTransitions
} from "jstests/concurrency/fsm_workload_helpers/state_transition_utils.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";

export const $config = (function() {
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

    // Keep data less then 64 (internalInsertMaxBatchSize) to avoid insertMany to yield while
    // inserting. This might cause an rename to execute during the insertMany and post-assertions
    // checks to fail.
    let data = {numChunks: 20, documentsPerChunk: 3, CRUDMutex: 'CRUDMutex'};

    /**
     * Used for mutual exclusion. Uses a collection to ensure atomicity on the read and update
     * operation.
     */
    function mutexLock(db, tid, collName) {
        jsTestLog('Trying to acquire mutexLock for resource tid:' + tid +
                  ' collection:' + collName);
        assert.soon(() => {
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
            const coll = db[targetThreadColl];
            const fullNs = coll.getFullName();
            jsTestLog('create state tid:' + tid + ' currentTid:' + this.tid +
                      ' collection:' + targetThreadColl);
            // Add necessary indexes for resharding.
            assert.commandWorked(db.adminCommand({
                createIndexes: targetThreadColl,
                indexes: [
                    {key: {[`tid_${tid}_0`]: 1}, name: `tid_${tid}_0_1`, unique: false},
                    {key: {[`tid_${tid}_1`]: 1}, name: `tid_${tid}_1_1`, unique: false}
                ],
                writeConcern: {w: 'majority'}
            }));
            try {
                assert.commandWorked(db.adminCommand(
                    {shardCollection: fullNs, key: {[`tid_${tid}_0`]: 1}, unique: false}));
            } catch (e) {
                const exceptionCode = e.code;
                if (exceptionCode) {
                    if (exceptionCode == ErrorCodes.ConflictingOperationInProgress ||
                        exceptionCode == ErrorCodes.AlreadyInitialized ||
                        exceptionCode == ErrorCodes.InvalidOptions) {
                        // It is fine for a shardCollection to throw AlreadyInitialized, a
                        // resharding state might have changed the shard key for the namespace. It
                        // is also fine to fail with InvalidOptions, a drop state could've removed
                        // the indexes and the CRUD state might have added some documents, forcing
                        // the need to manually create indexes.
                        return;
                    }
                }
                throw e;
            } finally {
                jsTestLog('create state finished');
            }
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
                assert.eq(db[targetThreadColl].drop(), true);
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
                assert.commandWorked(srcColl.renameCollection(destCollName));
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
        resharding: function(db, collName, connCache) {
            let tid = this.tid;
            // Pick a tid at random until we pick one that doesn't target this thread's collection.
            while (tid === this.tid)
                tid = Random.randInt(this.threadCount);
            const fullNs = db[threadCollectionName(collName, tid)].getFullName();
            let newKey = 'tid_' + tid + '_' + Random.randInt(2);
            try {
                jsTestLog('resharding state tid:' + tid + ' currentTid:' + this.tid +
                          ' collection:' + fullNs + ' newKey ' + newKey);
                assert.commandWorked(db.adminCommand(
                    {reshardCollection: fullNs, key: {[`${newKey}`]: 1}, numInitialChunks: 1}));
            } catch (e) {
                const exceptionCode = e.code;
                if (exceptionCode == ErrorCodes.ConflictingOperationInProgress ||
                    exceptionCode == ErrorCodes.ReshardCollectionInProgress ||
                    exceptionCode == ErrorCodes.NamespaceNotSharded ||
                    exceptionCode == ErrorCodes.NamespaceNotFound) {
                    // It is fine for a resharding operation to throw ConflictingOperationInProgress
                    // if a concurrent resharding with the same collection is ongoing.
                    // It is also fine for a resharding operation to throw NamespaceNotSharded or
                    // NamespaceNotFound because a drop state could've happend recently.
                    return;
                }
                throw e;
            } finally {
                jsTestLog('resharding state finished');
            }
        },
        checkDatabaseMetadataConsistency: function(db, collName, connCache) {
            jsTestLog('Check database metadata state');
            const inconsistencies = db.checkMetadataConsistency().toArray();
            assert.eq(0, inconsistencies.length, tojson(inconsistencies));
        },
        checkCollectionMetadataConsistency: function(db, collName, connCache) {
            let tid = this.tid;
            while (tid === this.tid)
                tid = Random.randInt(this.threadCount);

            const targetThreadColl = threadCollectionName(collName, tid);
            jsTestLog('Check collection metadata state tid:' + tid + ' currentTid:' + this.tid +
                      ' collection:' + targetThreadColl);
            const inconsistencies = db[targetThreadColl].checkMetadataConsistency().toArray();
            assert.eq(0, inconsistencies.length, tojson(inconsistencies));
        },
        untrackUnshardedCollection: function untrackUnshardedCollection(db, collName, connCache) {
            let tid = this.tid;
            while (tid === this.tid)
                tid = Random.randInt(this.threadCount);

            const targetThreadColl = threadCollectionName(collName, tid);
            // Note this command will behave as no-op in case the collection is not tracked.
            const namespace = `${db}.${targetThreadColl}`;
            jsTestLog(`Started to untrack collection ${namespace}`);
            assert.commandWorkedOrFailedWithCode(
                db.adminCommand({untrackUnshardedCollection: namespace}), [
                    // Handles the case where the collection is not located on its primary
                    ErrorCodes.OperationFailed,
                    // Handles the case where the collection is sharded
                    ErrorCodes.InvalidNamespace,
                    // Handles the case where the collection/db does not exist
                    ErrorCodes.NamespaceNotFound,
                    // The command does not exist in pre-8.0 versions
                    ErrorCodes.CommandNotFound,
                ]);
            jsTestLog(`Untrack collection completed`);
        },
        CRUD: function(db, collName, connCache) {
            let tid = this.tid;
            // Pick a tid at random until we pick one that doesn't target this thread's collection.
            while (tid === this.tid)
                tid = Random.randInt(this.threadCount);

            const targetThreadColl = threadCollectionName(collName, tid);
            const threadInfos =
                'tid:' + tid + ' currentTid:' + this.tid + ' collection:' + targetThreadColl;
            jsTestLog('CRUD state ' + threadInfos);
            const coll = db[targetThreadColl];

            mutexLock(db, tid, targetThreadColl);

            const generation = new Date().getTime();
            // Insert Data
            const numDocs = data.documentsPerChunk * data.numChunks;

            let insertBulkOp = coll.initializeUnorderedBulkOp();
            for (let i = 0; i < numDocs; ++i) {
                insertBulkOp.insert(
                    {generation: generation, count: i, [`tid_${tid}_0`]: i, [`tid_${tid}_1`]: i});
            }

            try {
                jsTestLog('CRUD - Insert ' + threadInfos);
                // Check if insert succeeded
                var res = insertBulkOp.execute();
                assert.commandWorked(res);

                let currentDocs = countDocuments(coll, {generation: generation});

                // Check guarantees IF NO CONCURRENT DROP is running.
                // If a concurrent rename came in, then either the full operation succeded (meaning
                // there will be 0 documents left) or the insert came in first.
                assert.contains(currentDocs, [0, numDocs], threadInfos);

                jsTestLog('CRUD - Update ' + threadInfos);
                res = coll.update({generation: generation}, {$set: {updated: true}}, {multi: true});
                if (res.hasWriteError()) {
                    var err = res.getWriteError();
                    if (err.code == ErrorCodes.QueryPlanKilled) {
                        // Update is expected to throw ErrorCodes::QueryPlanKilled if performed
                        // concurrently with a rename (SERVER-31695).
                        jsTestLog('CRUD state finished earlier because query plan was killed.');
                        return;
                    }
                    throw err;
                }
                assert.commandWorked(res, threadInfos);

                // Delete Data
                jsTestLog('CRUD - Remove ' + threadInfos);
                // Check if delete succeeded
                coll.remove({generation: generation}, {multi: true});
                // Check guarantees IF NO CONCURRENT DROP is running.
                assert.eq(countDocuments(coll, {generation: generation}), 0, threadInfos);
            } finally {
                mutexUnlock(db, tid, targetThreadColl);
            }
            jsTestLog('CRUD state finished');
        }
    };

    let setup = function(db, collName, cluster) {
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
