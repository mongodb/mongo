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

    /**
     * Used for mutual exclusion. Uses a collection to ensure atomicity on the read and update
     * operation.
     */
    function mutexLock(db, collName, tid) {
        assertAlways.soon(() => {
            let doc = db[collName].findAndModify({query: {tid: tid}, update: {$set: {mutex: 1}}});
            return doc.mutex === 0;
        });
    }

    function mutexUnlock(db, collName, tid) {
        db[collName].update({tid: tid}, {$set: {mutex: 0}});
    }

    let data = {numChunks: 20, documentsPerChunk: 5, CRUDMutex: 'CRUDMutex'};

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
            jsTestLog('create tid:' + tid + ' currentTid: ' + this.tid);
            assertAlways.commandWorked(
                db.adminCommand({shardCollection: fullNs, key: {_id: 1}, unique: false}));
        },
        drop: function(db, collName, connCache) {
            let tid = this.tid;
            // Pick a tid at random until we pick one that doesn't target this thread's collection.
            while (tid === this.tid)
                tid = Random.randInt(this.threadCount);

            jsTestLog('drop tid:' + tid + ' currentTid: ' + this.tid);
            mutexLock(db, data.CRUDMutex, tid);
            assertAlways.eq(db[threadCollectionName(collName, tid)].drop(), true);
            mutexUnlock(db, data.CRUDMutex, tid);
        },
        rename: function(db, collName, connCache) {
            let tid = this.tid;
            // Pick a tid at random until we pick one that doesn't target this thread's collection.
            while (tid === this.tid)
                tid = Random.randInt(this.threadCount);
            const srcColl = db[threadCollectionName(collName, tid)];
            const numInitialDocs = srcColl.countDocuments({});
            // Rename collection
            const destFullNs = threadCollectionName(collName, new Date().getTime());
            try {
                jsTestLog('rename tid:' + tid + ' currentTid: ' + this.tid);
                assertAlways.commandWorked(srcColl.renameCollection(destFullNs));
            } catch (e) {
                if (e.code && e.code === ErrorCodes.NamespaceNotFound) {
                    // TODO SERVER-54847: throw if ther is no renameCollection.start in changelog.
                    // The srcColl might've been dropped right before renaming it.
                    return;
                }
                throw e;
            }
        },
        CRUD: function(db, collName, connCache) {
            let tid = this.tid;
            // Pick a tid at random until we pick one that doesn't target this thread's collection.
            while (tid === this.tid)
                tid = Random.randInt(this.threadCount);

            const coll = db[threadCollectionName(collName, tid)];
            const fullNs = coll.getFullName();

            const generation = new Date().getTime();
            // Insert Data
            const numDocs = data.documentsPerChunk * data.numChunks;

            let insertBulkOp = coll.initializeUnorderedBulkOp();
            for (let i = 0; i < numDocs; ++i) {
                insertBulkOp.insert({generation: generation, count: i, tid: tid});
            }

            mutexLock(db, data.CRUDMutex, tid);
            jsTestLog('CRUD - Insert tid: ' + tid + ' currentTid: ' + this.tid);
            // Check if insert succeeded
            assertAlways.commandWorked(insertBulkOp.execute());
            let currentDocs = coll.countDocuments({generation: generation});
            // Check guarantees IF NO CONCURRENT DROP is running.
            // If a concurrent rename came in, then either the full operation succeded (meaning
            // there will be 0 documents left) or the insert came in first.
            assertAlways(currentDocs === numDocs || currentDocs === 0);

            jsTestLog('CRUD - Update tid: ' + tid + ' currentTid: ' + this.tid);
            assertAlways.commandWorked(
                coll.update({generation: generation}, {$set: {updated: true}}, {multi: true}));

            // Delete Data
            jsTestLog('CRUD - Remove tid: ' + tid + ' currentTid: ' + this.tid);
            // Check if delete succeeded
            coll.remove({generation: generation}, {multi: true});
            // Check guarantees IF NO CONCURRENT DROP is running.
            assertAlways.eq(coll.countDocuments({generation: generation}), 0);
            mutexUnlock(db, data.CRUDMutex, tid);
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
