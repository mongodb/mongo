/**
 * Performs a series of BulkWrite operations while DDL commands are running in the background
 * and verifies guarantees are not broken.
 *
 * @tags: [
 *   requires_sharding,
 *   assumes_balancer_off,
 *   does_not_support_causal_consistency,
 *   # The mutex mechanism used in BulkWrite and drop states does not support stepdown
 *   does_not_support_stepdowns,
 *   # Can be removed once PM-1965-Milestone-1 is completed.
 *   does_not_support_transactions,
 *   requires_fcv_80
 *  ]
 */

import {
    uniformDistTransitions
} from "jstests/concurrency/fsm_workload_helpers/state_transition_utils.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";

export const $config = (function() {
    function threadCollectionName(prefix, tid) {
        return [prefix + tid, prefix + tid + '_secondCollection'];
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

    let data = {numChunks: 20, documentsPerChunk: 5, BulkWriteMutex: 'BulkWriteMutex'};

    /**
     * Used for mutual exclusion. Uses a collection to ensure atomicity on the read and update
     * operation.
     */
    function mutexLock(db, tid, collName) {
        jsTestLog('Trying to acquire mutexLock for resource tid:' + tid +
                  ' collection:' + collName);
        assert.soon(() => {
            let doc = db[data.BulkWriteMutex].findAndModify(
                {query: {tid: tid}, update: {$set: {mutex: 1}}});
            return doc.mutex === 0;
        });
        jsTestLog('Acquired mutexLock for tid:' + tid + ' collection:' + collName);
    }

    function mutexUnlock(db, tid, collName) {
        db[data.BulkWriteMutex].update({tid: tid}, {$set: {mutex: 0}});
        jsTestLog('Unlocked lock for resource tid:' + tid + ' collection:' + collName);
    }

    let states = {
        init: function(db, collName, connCache) {
            this.collName = threadCollectionName(collName, this.tid)[0];
        },

        create: function(db, collName, connCache) {
            let tid = this.tid;
            // Pick a tid at random until we pick one that doesn't target this thread's collection.
            while (tid === this.tid)
                tid = Random.randInt(this.threadCount);

            const targetThreadColl = threadCollectionName(collName, tid)[0];
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
                    if (exceptionCode == ErrorCodes.AlreadyInitialized ||
                        exceptionCode == ErrorCodes.InvalidOptions) {
                        // It is fine for a shardCollection to throw AlreadyInitialized, a
                        // resharding state might have changed the shard key for the namespace. It
                        // is also fine to fail with InvalidOptions, a drop state could've removed
                        // the indexes and the BulkWrite state might have added some documents,
                        // forcing the need to manually create indexes.
                        return;
                    }
                }
                throw e;
            } finally {
                jsTestLog('create state finished tid: ' + tid);
            }
        },
        drop: function(db, collName, connCache) {
            let tid = this.tid;
            // Pick a tid at random until we pick one that doesn't target this thread's collection.
            while (tid === this.tid)
                tid = Random.randInt(this.threadCount);

            const targetThreadColl = threadCollectionName(collName, tid)[0];

            jsTestLog('drop state tid:' + tid + ' currentTid:' + this.tid +
                      ' collection:' + targetThreadColl);
            mutexLock(db, tid, targetThreadColl);
            try {
                assert.eq(db[targetThreadColl].drop(), true);
            } finally {
                mutexUnlock(db, tid, targetThreadColl);
            }
            jsTestLog('drop state finished tid: ' + tid);
        },
        rename: function(db, collName, connCache) {
            let tid = this.tid;
            // Pick a tid at random until we pick one that doesn't target this thread's collection.
            while (tid === this.tid)
                tid = Random.randInt(this.threadCount);
            const srcCollName = threadCollectionName(collName, tid)[0];
            const srcColl = db[srcCollName];
            // Rename collection
            const destCollName = collName + tid + '_' + extractUUIDFromObject(UUID());
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
                jsTestLog('rename state finished tid: ' + tid);
            }
        },
        resharding: function(db, collName, connCache) {
            let tid = this.tid;
            // Pick a tid at random until we pick one that doesn't target this thread's collection.
            while (tid === this.tid)
                tid = Random.randInt(this.threadCount);
            const fullNs = db[threadCollectionName(collName, tid)[0]].getFullName();
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
                    // It is also fine for a resharding operation to throw NamespaceNotSharded,
                    // because a drop state could've happend recently.
                    return;
                }
                throw e;
            } finally {
                jsTestLog('resharding state finished tid: ' + tid);
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

            const targetThreadColl = threadCollectionName(collName, tid)[0];
            jsTestLog('Check collection metadata state tid:' + tid + ' currentTid:' + this.tid +
                      ' collection:' + targetThreadColl);
            const inconsistencies = db[targetThreadColl].checkMetadataConsistency().toArray();
            assert.eq(0, inconsistencies.length, tojson(inconsistencies));
        },
        untrackUnshardedCollection: function untrackUnshardedCollection(db, collName, connCache) {
            // Note this command will behave as no-op in case the collection is not tracked.
            const namespace = `${db}.${collName}`;
            jsTestLog(`Started to untrack collection ${namespace}`);
            assert.commandWorkedOrFailedWithCode(
                db.adminCommand({untrackUnshardedCollection: namespace}), [
                    // Handles the case where the collection is not located on its primary
                    ErrorCodes.OperationFailed,
                    // Handles the case where the collection is sharded
                    ErrorCodes.InvalidNamespace,
                    // Handles the case where the collection/db does not exist
                    ErrorCodes.NamespaceNotFound,
                    //  TODO (SERVER-96072) remove this error once the command is backported.
                    ErrorCodes.CommandNotFound,
                ]);
            jsTestLog(`Untrack collection completed`);
        },
        BulkWrite: function(db, collName, connCache) {
            let tid = this.tid;
            // Pick a tid at random until we pick one that doesn't target this thread's collection.
            while (tid === this.tid)
                tid = Random.randInt(this.threadCount);

            const collNames = threadCollectionName(collName, tid);

            const fullNs1 = db[collNames[0]].getFullName();
            const fullNs2 = db[collNames[1]].getFullName();

            jsTestLog('BulkWrite state tid:' + tid + ' currentTid:' + this.tid +
                      ' collections:' + collNames);
            const coll = db[collNames[0]];

            const generation = new Date().getTime();
            // Insert Data
            const numDocs = data.documentsPerChunk * data.numChunks;

            let bulkWriteOps = [];
            for (let i = 0; i < numDocs; ++i) {
                bulkWriteOps.push({
                    insert: 0,
                    document:
                        {generation: generation, count: i, [`tid_${tid}_0`]: i, [`tid_${tid}_1`]: i}
                });
                bulkWriteOps.push({
                    insert: 1,
                    document:
                        {generation: generation, count: i, [`tid_${tid}_0`]: i, [`tid_${tid}_1`]: i}
                });
            }

            var bulkWriteCmd = {
                bulkWrite: 1,
                ops: bulkWriteOps,
                nsInfo: [{ns: fullNs1}, {ns: fullNs2}]
            };

            mutexLock(db, tid, collNames[0]);
            try {
                jsTestLog('BulkWrite - Insert tid:' + tid + ' currentTid:' + this.tid +
                          ' collection:' + collNames);

                // Assign txnNumber to enable retryable writes. This is necessary to avoid
                // an error caused by ShardCannotRefreshDueToLocksHeld in an interaction with
                // resharding where mongod will retry the entire command causing a duplicate key
                // error.
                const session = db.getSession();
                bulkWriteCmd = session._serverSession.assignTransactionNumber(bulkWriteCmd);

                // Check if insert succeeded
                var res = db.adminCommand(bulkWriteCmd);
                assert.commandWorked(res);
                assert.eq(res.nErrors,
                          0,
                          "BulkWrite - Insert errored when not expected to: " + tojson(res));

                jsTestLog('BulkWrite - Update tid:' + tid + ' currentTid:' + this.tid +
                          ' collection:' + collNames);
                bulkWriteOps = [];
                bulkWriteOps.push({
                    update: 0,
                    filter: {generation: generation},
                    updateMods: {$set: {updated: true}},
                    multi: true
                });
                bulkWriteOps.push({
                    update: 1,
                    filter: {generation: generation},
                    updateMods: {$set: {updated: true}},
                    multi: true
                });
                bulkWriteCmd = {
                    bulkWrite: 1,
                    ops: bulkWriteOps,
                    nsInfo: [{ns: fullNs1}, {ns: fullNs2}]
                };
                res = db.adminCommand(bulkWriteCmd);
                if (res.nErrors != 0) {
                    // Should only be possible for the first namespace to be renamed.
                    var err = res.cursor.firstBatch[0].code;
                    if (err == ErrorCodes.QueryPlanKilled) {
                        // Update is expected to throw ErrorCodes::QueryPlanKilled if performed
                        // concurrently with a rename (SERVER-31695).
                        jsTestLog(
                            'BulkWrite state finished earlier because query plan was killed.');
                        return;
                    }
                }
                assert.commandWorked(res);
                assert.eq(res.nErrors,
                          0,
                          "BulkWrite - Update errored when not expected to: " + tojson(res));

                // Delete Data
                jsTestLog('BulkWrite - Remove tid:' + tid + ' currentTid:' + this.tid +
                          ' collection:' + collNames);
                // Check if delete succeeded
                bulkWriteOps = [];
                bulkWriteOps.push({delete: 0, filter: {generation: generation}, multi: true});
                bulkWriteOps.push({delete: 1, filter: {generation: generation}, multi: true});
                bulkWriteCmd = {
                    bulkWrite: 1,
                    ops: bulkWriteOps,
                    nsInfo: [{ns: fullNs1}, {ns: fullNs2}]
                };
                res = db.adminCommand(bulkWriteCmd);
                assert.commandWorked(res);
                assert.eq(res.nErrors,
                          0,
                          "BulkWrite - Delete errored when not expected to: " + tojson(res));
                // Check guarantees IF NO CONCURRENT DROP is running.
                assert.eq(countDocuments(coll, {generation: generation}), 0);
            } finally {
                mutexUnlock(db, tid, collNames[0]);
            }
            jsTestLog('BulkWrite state finished tid: ' + tid);
        }
    };

    let initialReshardingMinimumOperationDurationMillis = null;

    let setup = function(db, collName, cluster) {
        for (let tid = 0; tid < this.threadCount; ++tid) {
            db[data.BulkWriteMutex].insert({tid: tid, mutex: 0});
        }
        // This test can time out on AUBSAN debug builds because sharding does not wait long enough
        // for other operations like retryable writes that started just after the resharding to
        // complete. Therefore, we force sharding to wait 15 seconds.
        cluster.executeOnConfigNodes((db) => {
            const res = assert.commandWorked(db.adminCommand(
                {setParameter: 1, reshardingMinimumOperationDurationMillis: 15000}));
            initialReshardingMinimumOperationDurationMillis = res.was;
        });
    };

    let teardown = function(db, collName, cluster) {
        cluster.executeOnConfigNodes((db) => {
            const res = assert.commandWorked(db.adminCommand({
                setParameter: 1,
                reshardingMinimumOperationDurationMillis:
                    initialReshardingMinimumOperationDurationMillis
            }));
        });
    };

    return {
        threadCount: 10,
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
