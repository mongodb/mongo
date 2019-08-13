'use strict';

/**
 * Runs refineCollectionShardKey and CRUD operations concurrently.
 *
 * @tags: [requires_sharding]
 */

load('jstests/libs/parallelTester.js');

var $config = (function() {
    // The organization of documents in every collection is as follows:
    //
    // (i)   Reserved for find:   {tid: tid, a:  0, b:  0} -->> {tid: tid, a: 24, b: 24}
    // (ii)  Reserved for remove: {tid: tid, a: 25, b: 25} -->> {tid: tid, a: 49, b: 49}
    // (iii) Reserved for update: {tid: tid, a: 50, b: 50} -->> {tid: tid, a: 74, b: 74}
    // (iv)  Reserved for insert: {tid: tid, a: 75, b: 75} -->> ...
    //
    // This organization prevents CRUD operations from interfering with one another and allows one
    // to assert for expected behavior.
    function insertDocs(data, coll) {
        const nDocsToInsert = data.latchCount * 3;
        let bulk = coll.initializeUnorderedBulkOp();

        for (let i = 0; i < nDocsToInsert; ++i) {
            bulk.insert({tid: data.tid, a: i, b: i});
        }

        const res = bulk.execute();
        assertAlways.commandWorked(res);
        assertAlways.eq(res.nInserted, nDocsToInsert);
    }

    const states = {
        init: function init(db, collName) {
            this.session = db.getMongo().startSession({retryWrites: true});
            this.sessionDB = this.session.getDatabase(db.getName());

            // Insert documents into all possible collections suffixed with this.latch.getCount()
            // that could receive CRUD operations over the course of the FSM workload.
            for (let i = this.latchCount; i >= 0; --i) {
                let coll = db.getCollection(collName + '_' + i);
                insertDocs(this, coll);
            }

            this.removeIdx = this.latchCount;
            this.updateIdx = this.latchCount * 2;
            this.insertIdx = this.latchCount * 3;
        },

        insert: function insert(db, collName) {
            // Randomly add 1 to this.latch.getCount() to increase the odds of inserting into a
            // collection that has just been refined.
            const collectionNumber = (Math.random() < 0.5)
                ? this.latch.getCount()
                : Math.min(this.latch.getCount() + 1, this.latchCount);

            const coll = db.getCollection(collName + '_' + collectionNumber);
            const res = coll.insert({tid: this.tid, a: this.insertIdx, b: this.insertIdx});

            assertAlways.commandWorked(res);
            assertAlways.eq(res.nInserted, 1);
            this.insertIdx++;
        },

        find: function find(db, collName) {
            // Randomly add 1 to this.latch.getCount() to increase the odds of finding in a
            // collection that has just been refined.
            const collectionNumber = (Math.random() < 0.5)
                ? this.latch.getCount()
                : Math.min(this.latch.getCount() + 1, this.latchCount);

            const idx = Random.randInt(this.latchCount);
            const coll = db.getCollection(collName + '_' + collectionNumber);
            const nFound = coll.find({tid: this.tid, a: idx, b: idx}).itcount();

            assertAlways.eq(nFound, 1);
        },

        update: function update(db, collName) {
            // Randomly add 1 to this.latch.getCount() to increase the odds of updating in a
            // collection that has just been refined.
            const collectionNumber = (Math.random() < 0.5)
                ? this.latch.getCount()
                : Math.min(this.latch.getCount() + 1, this.latchCount);

            const coll = this.sessionDB.getCollection(collName + '_' + collectionNumber);
            const res = coll.update({tid: this.tid, a: this.updateIdx, b: this.updateIdx},
                                    {tid: this.tid, a: this.insertIdx, b: this.insertIdx});

            assertAlways.commandWorked(res);
            assertAlways.eq(res.nMatched, 1);
            assertAlways.eq(res.nUpserted, 0);
            assertAlways.eq(res.nModified, 1);
            this.updateIdx++;
            this.insertIdx++;
        },

        remove: function remove(db, collName) {
            // Randomly add 1 to this.latch.getCount() to increase the odds of removing from a
            // collection that has just been refined.
            const collectionNumber = (Math.random() < 0.5)
                ? this.latch.getCount()
                : Math.min(this.latch.getCount() + 1, this.latchCount);

            const coll = db.getCollection(collName + '_' + collectionNumber);
            const res =
                coll.remove({tid: this.tid, a: this.removeIdx, b: this.removeIdx}, {justOne: true});

            assertAlways.commandWorked(res);
            assertAlways.eq(res.nRemoved, 1);
            this.removeIdx++;
        },

        refineCollectionShardKey: function refineCollectionShardKey(db, collName) {
            const coll = db.getCollection(collName + '_' + this.latch.getCount().toString());

            try {
                assertAlways.commandWorked(db.adminCommand(
                    {refineCollectionShardKey: coll.getFullName(), key: this.newShardKey}));
            } catch (e) {
                // There is a race that could occur where two threads run refineCollectionShardKey
                // concurrently on the same collection. Since the epoch of the collection changes,
                // the later thread may receive a StaleEpoch error, which is an acceptable error.
                if (e.code == ErrorCodes.StaleEpoch) {
                    print("Ignoring acceptable refineCollectionShardKey error: " + tojson(e));
                    return;
                }
                throw e;
            }

            this.latch.countDown();
        }
    };

    const transitions = {
        init: {insert: 0.2, find: 0.2, update: 0.2, remove: 0.2, refineCollectionShardKey: 0.2},
        insert: {insert: 0.2, find: 0.2, update: 0.2, remove: 0.2, refineCollectionShardKey: 0.2},
        find: {insert: 0.2, find: 0.2, update: 0.2, remove: 0.2, refineCollectionShardKey: 0.2},
        update: {insert: 0.2, find: 0.2, update: 0.2, remove: 0.2, refineCollectionShardKey: 0.2},
        remove: {insert: 0.2, find: 0.2, update: 0.2, remove: 0.2, refineCollectionShardKey: 0.2},
        refineCollectionShardKey:
            {insert: 0.2, find: 0.2, update: 0.2, remove: 0.2, refineCollectionShardKey: 0.2}
    };

    function setup(db, collName, cluster) {
        // Use a CountDownLatch as if it were a std::atomic<long long> shared between all of the
        // threads. The collection name is suffixed with the current this.latch.getCount() value
        // when concurrent CRUD operations are run against it. With every refineCollectionShardKey,
        // call this.latch.countDown() and run CRUD operations against the new collection suffixed
        // with this.latch.getCount(). This bypasses the need to drop and reshard the current
        // collection with every refineCollectionShardKey since it cannot be achieved in an atomic
        // fashion under the FSM infrastructure (meaning CRUD operations would fail).
        this.latchCount = this.iterations;
        this.latch = new CountDownLatch(this.latchCount);

        // Proactively create and shard all possible collections suffixed with this.latch.getCount()
        // that could receive CRUD operations over the course of the FSM workload. This prevents the
        // race that could occur between sharding a collection and creating an index on the new
        // shard key (if this step were done after every refineCollectionShardKey).
        for (let i = this.latchCount; i >= 0; --i) {
            let coll = db.getCollection(collName + '_' + i);
            assertAlways.commandWorked(
                db.adminCommand({shardCollection: coll.getFullName(), key: this.oldShardKey}));
            assertAlways.commandWorked(coll.createIndex(this.newShardKey));
        }
    }

    return {
        threadCount: 5,
        iterations: 25,
        startState: 'init',
        states: states,
        transitions: transitions,
        setup: setup,
        data: {oldShardKey: {a: 1}, newShardKey: {a: 1, b: 1}}
    };
})();