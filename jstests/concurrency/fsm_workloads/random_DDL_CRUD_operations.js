'use strict';

/**
 * Performs a series of CRUD operations while DDL commands are running in the background
 * and verifies guarantees are not broken.
 *
 * @tags: [
 *   requires_sharding,
 *   # TODO (SERVER-54881): ensure the new DDL paths work with balancer, autosplit
 *   # and causal consistency.
 *   assumes_balancer_off,
 *   assumes_autosplit_off,
 *   does_not_support_causal_consistency,
 *   # TODO (SERVER-54881): ensure the new DDL paths work with add/remove shards
 *   does_not_support_add_remove_shards,
 *   # TODO (SERVER-54905): ensure all DDL are resilient.
 *   does_not_support_stepdowns,
 *   # Can be removed once PM-1965-Milestone-1 is completed.
 *   does_not_support_transactions
 *  ]
 */

var $config = (function() {
    function threadCollectionName(prefix, tid) {
        return prefix + tid;
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
            assertAlways.eq(db[targetThreadColl].drop(), true);
            mutexUnlock(db, tid, targetThreadColl);
            jsTestLog('drop state finished');
        },
        rename: function(db, collName, connCache) {
            let tid = this.tid;
            // Pick a tid at random until we pick one that doesn't target this thread's collection.
            while (tid === this.tid)
                tid = Random.randInt(this.threadCount);
            const srcCollName = threadCollectionName(collName, tid);
            const srcColl = db[srcCollName];
            const numInitialDocs = srcColl.countDocuments({});
            // Rename collection
            const destCollName = threadCollectionName(collName, tid + '_' + new Date().getTime());
            try {
                jsTestLog('rename state tid:' + tid + ' currentTid:' + this.tid +
                          ' collection:' + srcCollName);
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
            jsTestLog('CRUD - Insert tid:' + tid + ' currentTid:' + this.tid +
                      ' collection:' + targetThreadColl);
            // Check if insert succeeded
            var res = insertBulkOp.execute();
            assertAlways.commandWorked(res);

            let currentDocs = coll.countDocuments({generation: generation});
            // Check guarantees IF NO CONCURRENT DROP is running.
            // If a concurrent rename came in, then either the full operation succeded (meaning
            // there will be 0 documents left) or the insert came in first.
            assertAlways(currentDocs === numDocs || currentDocs === 0);

            jsTestLog('CRUD - Update tid:' + tid + ' currentTid:' + this.tid +
                      ' collection:' + targetThreadColl);
            var res = coll.update({generation: generation}, {$set: {updated: true}}, {multi: true});
            if (res.hasWriteError()) {
                var err = res.getWriteError();
                if (err.code == ErrorCodes.QueryPlanKilled) {
                    // Update is expected to throw ErrorCodes::QueryPlanKilled if performed
                    // concurrently with a rename (SERVER-31695).
                    mutexUnlock(db, tid, targetThreadColl);
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
            assertAlways.eq(coll.countDocuments({generation: generation}), 0);
            mutexUnlock(db, tid, targetThreadColl);
            jsTestLog('CRUD state finished');
        }
    };

    let setup = function(db, collName, connCache) {
        for (let tid = 0; tid < this.threadCount; ++tid) {
            db[data.CRUDMutex].insert({tid: tid, mutex: 0});
        }
    };

    let transitions = {
        init: {create: 1.0},
        create: {create: 0.01, CRUD: 0.33, drop: 0.33, rename: 0.33},
        CRUD: {create: 0.33, CRUD: 0.01, drop: 0.33, rename: 0.33},
        drop: {create: 0.33, CRUD: 0.33, drop: 0.01, rename: 0.33},
        rename: {create: 0.33, CRUD: 0.33, drop: 0.33, rename: 0.01}
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
