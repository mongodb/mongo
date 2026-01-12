/**
 * Tests $lookup with untracked-unsharded, tracked-unsharded, and sharded collections while
 * movePrimary operations are occurring on the database. On suites with random balancer enabled,
 * additionally moveChunk and moveCollection operations are occurring.
 *
 * @tags: [requires_sharding]
 */
import {interruptedQueryErrors} from "jstests/concurrency/fsm_libs/assert.js";

export const $config = (function() {
    const data = {numDocs: 100};

    function retryOnInterruptedQueryError(fn, numRetries, sleepMs) {
        for (let i = 0; i < numRetries; ++i) {
            try {
                return fn();
            } catch (e) {
                if (interruptedQueryErrors.includes(e.code)) {
                    jsTestLog(`Caught interrupted query error ${e.code}. Retrying ${i + 1}/${
                        numRetries}...`);
                    sleep(sleepMs);
                    continue;
                }
                throw e;
            }
        }
    }

    function runAggWithRetries(db, collName, pipeline) {
        return retryOnInterruptedQueryError(
            () => {
                return db[collName].aggregate(pipeline).toArray();
            },
            10 /* numRetries */,
            10 /* sleepMs */,
        );
    }

    const states = (function() {
        function lookupTwoCollections(db, _) {
            const results = runAggWithRetries(db, this.localCollName, [
                {
                    $match: {x: Random.randInt(this.numDocs)},
                },
                {
                    $lookup: {
                        from: this.foreignColl1Name,
                        localField: "_id",
                        foreignField: "_id",
                        as: "out",
                    },
                },
            ]);

            assert.eq(results.length, 1, results);
            const resultDoc = results[0];
            assert.eq(resultDoc.out.length, 1, results);
            assert.eq(resultDoc._id, resultDoc.out[0]._id, results);
            assert.eq(resultDoc.x, resultDoc.out[0].y, results);
        }

        function lookupThreeCollections(db, _) {
            const x_val = Random.randInt(this.numDocs);
            const results = runAggWithRetries(db, this.localCollName, [
                {$match: {x: x_val}},
                {
                    $lookup: {
                        from: this.foreignColl1Name,
                        localField: "x",
                        foreignField: "y",
                        as: "j",
                    },
                },
                {$unwind: "$j"},
                {
                    $lookup: {
                        from: this.foreignColl2Name,
                        localField: "j.y",
                        foreignField: "z",
                        as: "j.j2",
                    },
                },
                {$unwind: "$j.j2"},
            ]);

            assert.eq(results.length, 1, results);
            const resultDoc = results[0];
            assert.eq(resultDoc.x, x_val, results);
            assert.eq(resultDoc.j.y, x_val, results);
            assert.eq(resultDoc.j.j2.z, x_val, results);
        }

        function movePrimary(db, _) {
            // Let only one thread do movePrimary to avoid threads stalling behind each other.
            if (this.tid !== 0) {
                jsTestLog("Skipping movePrimary on thread " + this.threadId);
                return;
            }

            const toShard = this.shards[Random.randInt(this.shards.length)];

            jsTestLog("Executing movePrimary to shard: " + toShard);
            retryOnRetryableError(
                () => {
                    assert.commandWorked(db.adminCommand({movePrimary: db.getName(), to: toShard}));
                },
                10 /* numRetries */,
                100 /* sleepMs */,
                [
                    // The cloning phase has failed (e.g. as a result of a stepdown). When a failure
                    // occurs at this phase, the movePrimary operation does not recover.
                    7120202,
                ],
            );
        }

        return {lookupTwoCollections, lookupThreeCollections, movePrimary};
    })();

    const transitions = {
        lookupTwoCollections: {
            lookupTwoCollections: 0.45,
            lookupThreeCollections: 0.45,
            movePrimary: 0.1,
        },
        lookupThreeCollections: {
            lookupTwoCollections: 0.45,
            lookupThreeCollections: 0.45,
            movePrimary: 0.1,
        },
        movePrimary: {
            lookupTwoCollections: 0.45,
            lookupThreeCollections: 0.45,
            movePrimary: 0.1,
        },
    };

    function setup(db, _, cluster) {
        this.shards = Object.keys(cluster.getSerializedCluster().shards);
        const shards = this.shards;

        this.localCollName = "localColl";
        this.foreignColl1Name = "foreignColl1";
        this.foreignColl2Name = "foreignColl2";

        function createShardedCollection(collName) {
            assert.commandWorked(db.adminCommand(
                {shardCollection: db[collName].getFullName(), key: {_id: "hashed"}}));
        }

        function createTrackedCollection(collName) {
            db.createCollection(collName);
            // move collection to a random shard
            const toShard = shards[Random.randInt(shards.length)];
            jsTestLog("Creating tracked collection: " + collName + " on shard " + toShard);
            assert.soonRetryOnAcceptableErrors(() => {
                assert.commandWorked(db.adminCommand(
                    {moveCollection: db.getName() + "." + collName, toShard: toShard}));
                return true;
            }, [ErrorCodes.ConflictingOperationInProgress, ErrorCodes.ReshardCollectionInProgress]);
        }

        function createRandomCollectionType(collName) {
            const randType = Random.randInt(3);
            if (randType === 0) {
                jsTestLog("Creating sharded collection: " + collName);
                createShardedCollection(collName);
            } else if (randType === 1) {
                createTrackedCollection(collName);
            } else {
                jsTestLog("Creating untracked collection: " + collName);
                // noop
            }
        }

        // Load local collection data.
        {
            createRandomCollectionType(this.localCollName);

            const bulk = db[this.localCollName].initializeUnorderedBulkOp();
            for (let i = 0; i < this.numDocs; ++i) {
                bulk.insert({_id: i, x: i});
            }
            const res = bulk.execute();
            assert.commandWorked(res);
            assert.eq(this.numDocs, res.nInserted);
        }

        // Load foreign collection data.
        {
            createRandomCollectionType(this.foreignColl1Name);

            const bulk = db[this.foreignColl1Name].initializeUnorderedBulkOp();
            for (let i = 0; i < this.numDocs; ++i) {
                bulk.insert({_id: i, y: i});
            }
            const res = bulk.execute();
            assert.commandWorked(res);
            assert.eq(this.numDocs, res.nInserted);
        }

        // Load foreign collection 2 data.
        {
            createRandomCollectionType(this.foreignColl2Name);

            const bulk = db[this.foreignColl2Name].initializeUnorderedBulkOp();
            for (let i = 0; i < this.numDocs; ++i) {
                bulk.insert({_id: i, z: i});
            }
            const res = bulk.execute();
            assert.commandWorked(res);
            assert.eq(this.numDocs, res.nInserted);
        }
    }

    function teardown(db, collName, cluster) {
    }

    return {
        threadCount: 5,
        iterations: 50,
        states: states,
        startState: "lookupTwoCollections",
        transitions: transitions,
        data: data,
        setup: setup,
        teardown: teardown,
    };
})();
