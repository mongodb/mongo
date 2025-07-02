/**
 * Runs many raw data writes and queries against a time-series collection at the same
 * time.
 *
 * @tags: [
 *  requires_timeseries,
 *  requires_fcv_82,
 *  # Time-series collections cannot be written to in a transaction.
 *  does_not_support_transactions,
 *  # Time-series findAndModify does not support retryable writes.
 *  requires_non_retryable_writes,
 *  requires_getmore,
 * ]
 */

export const $config = (function() {
    const threadCount = 20;
    // These buckets are not deleted during the test.
    const initialBucketCount = 100;

    const data = {
        addedBucketCount: 0,

        timeFieldName: "time",
        metaFieldName: "meta",
        thisThreadKey: "meta.thread",

        getSideCollectionName: function() {
            return "side_" + jsTestName();
        },

        getMainCollection: function(db) {
            return db.getCollection(jsTestName());
        },

        getSideCollection: function(db) {
            return db.getCollection(this.getSideCollectionName());
        },

        nextMetaFieldCounter: 0,
        getNextMetaField: function() {
            return {
                nextMetaFieldCounter: this.nextMetaFieldCounter++,
                thread: this.tid,
            };
        },

        getNewBucket: function(db) {
            const sideCollection = this.getSideCollection(db);
            const meta = this.getNextMetaField();
            sideCollection.insert({
                [this.timeFieldName]: new Date(),
                [this.metaFieldName]: meta,
                data: Random.rand(),
            });
            return sideCollection.findOne(
                {[this.metaFieldName]: meta}, null, null, null, null, true /* rawData */);
        },
    };

    const states = {
        findInitial: function findInitial(db, collname) {
            const coll = this.getMainCollection(db);
            const res = coll.find({"meta.initial": {$exists: true}}).rawData();
            assert.eq(res.count(), initialBucketCount);
        },

        findAll: function findAll(db, collName) {
            const coll = this.getMainCollection(db);
            const res = coll.find().rawData();
            for (const bucket of res.toArray()) {
                assert(bucket.hasOwnProperty("control"));
            }
        },

        addBucketByMeasurement: function addBucketByMeasurement(db, collName) {
            const coll = this.getMainCollection(db);
            const res = coll.insert({
                [this.metaFieldName]: this.getNextMetaField(),
                [this.timeFieldName]: new Date(),
                data: Random.rand(),
            });
            assert.commandWorked(res);
            this.addedBucketCount += res.nInserted;
        },

        insert: function insert(db, collName) {
            const coll = this.getMainCollection(db);
            const bucket = this.getNewBucket(db);
            const res = coll.insert(bucket, {rawData: true});
            assert.commandWorked(res);
            this.addedBucketCount += res.nInserted;
        },

        deleteOne: function deleteOne(db, collName) {
            const coll = this.getMainCollection(db);
            const res = coll.deleteOne({
                [this.thisThreadKey]: this.tid,
            },
                                       {rawData: true});
            assert.commandWorked(res);
            this.addedBucketCount -= res.deletedCount;
        },

        count: function count(db, collName) {
            const coll = this.getMainCollection(db);
            const count = coll.count({[this.thisThreadKey]: this.tid}, {rawData: true});
            assert.eq(count,
                      this.addedBucketCount,
                      `T${this.tid} - count - expected ${this.addedBucketCount}, got ${count}`);
        },

        aggregate: function aggregate(db, collName) {
            const coll = this.getMainCollection(db);
            const agg = coll.aggregate(
                [
                    {$match: {[this.thisThreadKey]: this.tid}},
                ],
                {rawData: true});
            assert.eq(agg.toArray().length,
                      this.addedBucketCount,
                      `T${this.tid} - Expected ${this.addedBucketCount} buckets, got:
                          ${tojson(this.getMainCollection(db).find().rawData().toArray())}`);
        },

        distinct: function distinct(db, collName) {
            const coll = this.getMainCollection(db);
            // While there can be as many values for `thisThreadKey` as there were FSM
            // threads, it's possible that some threads have transiently deleted all of their
            // buckets, so we can only place an upper bound.
            const numNonSeedBuckets = coll.distinct(this.thisThreadKey, {}, {rawData: true}).length;
            assert.lte(numNonSeedBuckets,
                       threadCount,
                       `T${this.tid} - Expected at most ${
                           threadCount} distinct non-seed meta fields, got ${numNonSeedBuckets}`);

            const numBuckets = coll.distinct("control", {}, {rawData: true}).length;
            assert.gte(numBuckets,
                       initialBucketCount,
                       `T${this.tid} - Expected at least ${
                           initialBucketCount} distinct bucket document, got ${numBuckets}`);
        },
    };

    function setup(db, collName, cluster) {
        // This collection is for generating buckets to be inserted into the main collection.
        assert.commandWorked(db.createCollection(this.getSideCollectionName(), {
            timeseries: {
                timeField: this.timeFieldName,
                metaField: this.metaFieldName,
            }
        }));

        // This collection is for the FSM test.
        assert.commandWorked(db.createCollection(jsTestName(), {
            timeseries: {
                timeField: this.timeFieldName,
                metaField: this.metaFieldName,
            }
        }));

        const bulk = this.getMainCollection(db).initializeOrderedBulkOp();

        for (let i = 0; i < initialBucketCount; i++) {
            bulk.insert({
                [this.timeFieldName]: new Date(),
                [this.metaFieldName]: {initial: i},
                data: Random.rand(),
            });
        }
        assert.commandWorked(bulk.execute());
    };

    function teardown(db, collName, cluster) {
        const coll = this.getMainCollection(db);
        assert.eq(coll.find({"meta.initial": 1}).rawData().length(), 1);
        const buckets = coll.find().rawData().toArray();
        for (let bucket of buckets) {
            assert.eq(bucket.control.count, 1);
        }
    };

    const standardTransition = {
        findInitial: 1,
        findAll: 1,
        addBucketByMeasurement: 1,
        insert: 1,
        deleteOne: 1,
        count: 1,
        aggregate: 1,
        distinct: 1,
    };

    const transitions = {
        findInitial: standardTransition,
        findAll: standardTransition,
        addBucketByMeasurement: standardTransition,
        insert: standardTransition,
        deleteOne: standardTransition,
        count: standardTransition,
        aggregate: standardTransition,
        distinct: standardTransition,
    };

    return {
        threadCount: threadCount,
        iterations: 100,
        startState: "insert",
        data: data,
        states: states,
        transitions: transitions,
        teardown: teardown,
        setup: setup,
    };
})();
