/**
 * Runs refineCollectionShardKey and CRUD operations concurrently.
 *
 * @tags: [requires_persistence, requires_sharding]
 */
import "jstests/libs/parallelTester.js";

export const $config = (function () {
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
            bulk.insert(data.usingNestedKey ? {tid: data.tid, a: i, b: {c: i}} : {tid: data.tid, a: i, b: i});
        }

        const res = bulk.execute();
        assert.commandWorked(res);
        assert.eq(res.nInserted, nDocsToInsert);
    }

    const states = {
        init: function init(db, collName) {
            this.session = db.getMongo().startSession({retryWrites: true});
            this.sessionDB = this.session.getDatabase(db.getName());

            // Insert documents into all possible collections suffixed with this.latch.getCount()
            // that could receive CRUD operations over the course of the FSM workload.
            for (let i = this.latchCount; i >= 0; --i) {
                let coll = db.getCollection(collName + "_" + i);
                insertDocs(this, coll);
            }

            this.removeIdx = this.latchCount;
            this.updateIdx = this.latchCount * 2;
            this.insertIdx = this.latchCount * 3;
        },

        insert: function insert(db, collName) {
            // Randomly add 1 to this.latch.getCount() to increase the odds of inserting into a
            // collection that has just been refined.
            const collectionNumber =
                Math.random() < 0.5 ? this.latch.getCount() : Math.min(this.latch.getCount() + 1, this.latchCount);

            const coll = db.getCollection(collName + "_" + collectionNumber);
            const res = this.usingNestedKey
                ? coll.insert({tid: this.tid, a: this.insertIdx, b: {c: this.insertIdx}})
                : coll.insert({tid: this.tid, a: this.insertIdx, b: this.insertIdx});

            assert.commandWorked(res);
            assert.eq(res.nInserted, 1);
            this.insertIdx++;
        },

        find: function find(db, collName) {
            // Randomly add 1 to this.latch.getCount() to increase the odds of finding in a
            // collection that has just been refined.
            const collectionNumber =
                Math.random() < 0.5 ? this.latch.getCount() : Math.min(this.latch.getCount() + 1, this.latchCount);

            const idx = Random.randInt(this.latchCount);
            const coll = db.getCollection(collName + "_" + collectionNumber);
            const nFound = this.usingNestedKey
                ? coll.find({tid: this.tid, a: idx, b: {c: idx}}).itcount()
                : coll.find({tid: this.tid, a: idx, b: idx}).itcount();

            assert.eq(nFound, 1);
        },

        update: function update(db, collName) {
            // Randomly add 1 to this.latch.getCount() to increase the odds of updating in a
            // collection that has just been refined.
            const collectionNumber =
                Math.random() < 0.5 ? this.latch.getCount() : Math.min(this.latch.getCount() + 1, this.latchCount);

            const coll = this.sessionDB.getCollection(collName + "_" + collectionNumber);
            const res = this.usingNestedKey
                ? coll.update(
                      {tid: this.tid, a: this.updateIdx, b: {c: this.updateIdx}},
                      {tid: this.tid, a: this.insertIdx, b: {c: this.insertIdx}},
                  )
                : coll.update(
                      {tid: this.tid, a: this.updateIdx, b: this.updateIdx},
                      {tid: this.tid, a: this.insertIdx, b: this.insertIdx},
                  );

            if (res.hasWriteError()) {
                let err = res.getWriteError();
                if (
                    TestData.runningWithBalancer &&
                    err.code === ErrorCodes.DuplicateKey &&
                    err.errmsg.includes("Failed to update document's shard key field")
                ) {
                    // If we are running with the balancer disabled, we might update the document so
                    // that it moves to a shard which contains an orphaned version of the document.
                    jsTest.log("Ignoring DuplicateKey error during update due to ongoing migrations");
                    return;
                }

                const transientErrorCodes = new Set([
                    ErrorCodes.LockTimeout,
                    ErrorCodes.StaleConfig,
                    ErrorCodes.ConflictingOperationInProgress,
                    ErrorCodes.ShardCannotRefreshDueToLocksHeld,
                    ErrorCodes.WriteConflict,
                    ErrorCodes.SnapshotUnavailable,
                    ErrorCodes.ExceededTimeLimit,
                ]);
                if (
                    transientErrorCodes.has(err.code) &&
                    err.errmsg.includes("was converted into a distributed transaction")
                ) {
                    jsTest.log(
                        "Ignoring transient error during an update operation after a WouldChangeOwningShard error",
                    );
                    return;
                }
            }

            assert.commandWorked(res);
            assert.eq(res.nMatched, 1);
            assert.eq(res.nUpserted, 0);
            assert.eq(res.nModified, 1);
            this.updateIdx++;
            this.insertIdx++;
        },

        remove: function remove(db, collName) {
            // Randomly add 1 to this.latch.getCount() to increase the odds of removing from a
            // collection that has just been refined.
            const collectionNumber =
                Math.random() < 0.5 ? this.latch.getCount() : Math.min(this.latch.getCount() + 1, this.latchCount);

            const coll = db.getCollection(collName + "_" + collectionNumber);
            const res = this.usingNestedKey
                ? coll.remove({tid: this.tid, a: this.removeIdx, b: {c: this.removeIdx}}, {justOne: true})
                : coll.remove({tid: this.tid, a: this.removeIdx, b: this.removeIdx}, {justOne: true});

            assert.commandWorked(res);
            assert.eq(res.nRemoved, 1);
            this.removeIdx++;
        },

        refineCollectionShardKey: function refineCollectionShardKey(db, collName) {
            const coll = db.getCollection(collName + "_" + this.latch.getCount().toString());

            try {
                assert.commandWorked(
                    db.adminCommand({refineCollectionShardKey: coll.getFullName(), key: this.newShardKey}),
                );
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
        },

        // Occasionally flush the router's cached metadata to verify the metadata for the refined
        // collections can be successfully loaded.
        flushRouterConfig: function flushRouterConfig(db, collName) {
            assert.commandWorked(db.adminCommand({flushRouterConfig: db.getName()}));
        },
    };

    const transitions = {
        init: {insert: 0.2, find: 0.2, update: 0.2, remove: 0.2, refineCollectionShardKey: 0.2},
        insert: {
            insert: 0.18,
            find: 0.18,
            update: 0.18,
            remove: 0.18,
            refineCollectionShardKey: 0.18,
            flushRouterConfig: 0.1,
        },
        find: {
            insert: 0.18,
            find: 0.18,
            update: 0.18,
            remove: 0.18,
            refineCollectionShardKey: 0.18,
            flushRouterConfig: 0.1,
        },
        update: {
            insert: 0.18,
            find: 0.18,
            update: 0.18,
            remove: 0.18,
            refineCollectionShardKey: 0.18,
            flushRouterConfig: 0.1,
        },
        remove: {
            insert: 0.18,
            find: 0.18,
            update: 0.18,
            remove: 0.18,
            refineCollectionShardKey: 0.18,
            flushRouterConfig: 0.1,
        },
        refineCollectionShardKey: {
            insert: 0.18,
            find: 0.18,
            update: 0.18,
            remove: 0.18,
            refineCollectionShardKey: 0.18,
            flushRouterConfig: 0.1,
        },
        flushRouterConfig: {insert: 0.2, find: 0.2, update: 0.2, remove: 0.2, refineCollectionShardKey: 0.2},
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
            let coll = db.getCollection(collName + "_" + i);
            assert.commandWorked(db.adminCommand({shardCollection: coll.getFullName(), key: this.oldShardKey}));
            assert.commandWorked(coll.createIndex(this.newShardKey));
        }
    }

    return {
        threadCount: 5,
        iterations: 25,
        startState: "init",
        states: states,
        transitions: transitions,
        setup: setup,
        data: {
            newShardKey: {a: 1, b: 1},
            oldShardKey: {a: 1},
            usingNestedKey: false,
        },
    };
})();
