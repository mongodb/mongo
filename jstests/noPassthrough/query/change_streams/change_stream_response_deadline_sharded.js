/**
 * Tests that in a sharded cluster change stream, the 'operationResponseMaxMS' query knob set via
 * query settings is forwarded to shard mongods and honored, causing getMore operations to return
 * partial results with advancing post-batch resume tokens (PBRTs).
 *
 * This is the sharded counterpart of jstests/change_streams/change_stream_response_deadline.js.
 * The test verifies that query settings are correctly propagated to shard-level change stream
 * cursors via createUpdatedCommandForNewShard(), which builds the command sent to each shard.
 *
 * Test cases:
 *   1. Unsharded collection on a single shard - validates basic query settings forwarding.
 *   2. Sharded collection spanning two shards - validates that each shard receives the settings
 *      regardless of which shard hosts a given chunk.
 *   3. $_passthroughToShard targeting a specific shard - validates the kSpecificShardOnly code
 *      path in cluster_aggregate.cpp (serializeForPassthrough -> addQuerySettingsToRequest).
 *   4. Dynamic shard addition - validates that operationResponseMaxMS is forwarded to a cursor
 *      opened on a new shard after the change stream has been established, via the
 *      createUpdatedCommandForNewShard() path triggered by a newShardDetected event.
 *
 * Each test case also verifies that log message 10290000 ("Interrupting long-running collection
 * scan after configured deadline") is emitted on each targeted shard, confirming that the knob
 * value reached the shard executor and the deadline was actually applied.
 *
 * @tags: [
 *   assumes_balancer_off,
 *   featureFlagAllowUserFacingQuerySettings,
 *   featureFlagPqsQueryKnobs,
 *   requires_fcv_90,
 *   requires_sharding,
 *   uses_change_streams,
 * ]
 */
import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {
    ChangeStreamTest,
    getClusterTime,
    withBalancerEnabled,
} from "jstests/libs/query/change_stream_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("sharded change stream operationResponseMaxMS via query settings", () => {
    // kDelayMS must exceed kScanMaxMS so that a single CollectionScan::doWork() call exhausts
    // the scan budget on the very next yield check.
    const kScanMaxMS = 250;
    const kDelayMS = 300;

    // kNumDocs inserts create kNumDocs + 1 oplog entries (createCollection + kNumDocs inserts).
    // The matching document is the last insert (_id: kNumDocs - 1). With kDelayMS > kScanMaxMS,
    // each scanned entry causes the budget to expire after one doWork(), so we expect at least
    // kNumDocs empty-batch getMores before the matching document is returned.
    const kNumDocs = 10;
    const kTargetId = kNumDocs - 1;

    // Log ID emitted by PlanExecutorImpl when the operationResponseMaxMS deadline is exceeded
    // and the executor interrupts the collection scan to return a partial result.
    const kResponseDeadlineLogId = 10290000;

    const dbName = jsTestName();

    // Options to add to the '$changeStream' stage for testing v1 and v2 change stream readers.
    const changeStreamOptions = {
        v1: {version: "v1"},
        v2: {},
    };

    let st;
    let mongos;
    let testDB;
    let cst;

    const insertDocs = (coll) => {
        // Insert documents one by one, so that there are individual insert oplog entries and
        // not a single applyOps oplog entry.
        for (let i = 0; i < kNumDocs; ++i) {
            assert.commandWorked(coll.insert({_id: i}));
        }
    };

    // Wraps cb() with the hangCollScanDoWork failpoint active on all shard mongod hosts.
    const runWithFailPoint = (cb) => {
        const failPoints = [
            configureFailPoint(st.rs0.getPrimary(), "hangCollScanDoWork", {delay: kDelayMS}),
            configureFailPoint(st.rs1.getPrimary(), "hangCollScanDoWork", {delay: kDelayMS}),
        ];
        try {
            return cb();
        } finally {
            failPoints.forEach((fp) => fp.off());
        }
    };

    // Runs the core assertion: opens a change stream at startTime with batchSize 0 and the
    // operationResponseMaxMS query knob set via query settings, then issues getMores until the
    // matching document (_id: kNumDocs - 1) is returned. Asserts that at least 'minDifferentPBRTs'
    // distinct PBRTs were observed before the document arrived, confirming that the knob was honored
    // on the shard. For an unsharded collection the oplog contains createCollection + (kNumDocs - 1)
    // non-matching inserts before the match, so 5 is a safe lower bound. For a sharded collection
    // the two shards scan in lockstep and the effective number of client-visible empty batches is
    // lower (~half of kNumDocs), so a smaller minimum is used.
    // extraAggregateOptions merges into the aggregate command (e.g. {$_passthroughToShard: ...}).
    // shardsToVerify is an array of shard primary connections whose logs are checked for log ID
    // kResponseDeadlineLogId, confirming the knob reached the shard executor.
    const runTest = (
        cst,
        collName,
        startTime,
        shardsToVerify,
        minDifferentPBRTs,
        extraAggregateOptions = {},
        changeStreamOptions = {},
    ) => {
        // Clear each shard's log and capture the resulting (near-zero) offset. This prevents
        // the fixed-size log ring buffer from evicting earlier entries before we can read them.
        shardsToVerify.forEach((s) => s.adminCommand({clearLog: "global"}));
        const logOffsets = shardsToVerify.map((s) => checkLog.getGlobalLog(s).length);

        runWithFailPoint(() => {
            const cursor = cst.startWatchingChanges({
                pipeline: [
                    {
                        $changeStream: Object.assign(
                            {startAtOperationTime: startTime},
                            changeStreamOptions,
                        ),
                    },
                    {$match: {"fullDocument._id": kTargetId}},
                ],
                collection: collName,
                aggregateOptions: Object.merge({cursor: {batchSize: 0}}, extraAggregateOptions),
                querySettings: {queryKnobs: {operationResponseMaxMS: NumberLong(kScanMaxMS)}},
            });

            const initialPBRT = cursor.postBatchResumeToken;
            assert(initialPBRT, "Expected a postBatchResumeToken in the aggregate response", {
                cursor,
            });

            let differentPBRTsReturned = 0;
            let foundDoc = null;
            let lastPBRT = initialPBRT;

            for (let i = 0; i < 100 && foundDoc === null; i++) {
                cst.getNextBatch(cursor);

                const batch = cursor.nextBatch;
                const pbrt = cursor.postBatchResumeToken;

                assert(pbrt, `Expected a postBatchResumeToken in getMore response ${i}`, {cursor});

                const pbrtCompareResult = bsonWoCompare(lastPBRT, pbrt);
                assert.lte(pbrtCompareResult, 0, "PBRT must be monotonically non-decreasing");
                if (batch.length === 0) {
                    assert.neq(
                        NumberLong(0),
                        cursor.id,
                        "Cursor must stay open after an empty batch",
                        {cursor},
                    );
                } else {
                    assert.eq(
                        1,
                        batch.length,
                        "Expected exactly one document when the scan finds a match",
                        {cursor},
                    );
                    foundDoc = batch[0];
                }
                if (pbrtCompareResult < 0) {
                    lastPBRT = pbrt;
                    differentPBRTsReturned++;
                }
            }

            assert(foundDoc !== null, "Did not receive the matching document within the limit");
            assert.eq(
                kTargetId,
                foundDoc.fullDocument._id,
                "Expected the last inserted document (_id: kNumDocs-1)",
                {foundDoc},
            );
            assert.gte(
                differentPBRTsReturned,
                minDifferentPBRTs,
                "Expected multiple empty batches with distinct PBRTs before the matching document," +
                    " indicating that operationResponseMaxMS was forwarded to the shard",
                {differentPBRTsReturned, minDifferentPBRTs},
            );
        });

        // Verify that the deadline log was emitted on every targeted shard, confirming the knob
        // reached each shard's plan executor and was actually applied.
        shardsToVerify.forEach((shard, i) => {
            const offset = logOffsets[i];
            assert.soon(
                () =>
                    checkLog
                        .getGlobalLog(shard)
                        .slice(offset)
                        .some((log) => {
                            try {
                                return JSON.parse(log).id === kResponseDeadlineLogId;
                            } catch (e) {
                                return false;
                            }
                        }),
                `Expected log ${kResponseDeadlineLogId} on shard ${shard.host} indicating ` +
                    "operationResponseMaxMS was forwarded and honored",
            );
        });
    };

    before(() => {
        st = new ShardingTest({
            shards: 2,
            mongos: 1,
            rs: {nodes: 1, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}},
            other: {
                enableBalancer: false,
                configOptions: {
                    setParameter: {
                        writePeriodicNoops: true,
                        periodicNoopIntervalSecs: 1,
                    },
                },
            },
        });
        mongos = st.s;
        testDB = mongos.getDB(dbName);

        // Enable verbosity 3 for the query component on each shard primary so that log ID
        // kResponseDeadlineLogId (emitted at debug level 3) is visible in the shard logs.
        for (const rs of [st.rs0, st.rs1]) {
            assert.commandWorked(
                rs.getPrimary().adminCommand({
                    setParameter: 1,
                    logComponentVerbosity: {query: {verbosity: 3}},
                }),
            );
        }

        // Explicitly set shard0 as the primary shard for the test database. This ensures that all
        // unsharded collections land on shard0, so that $_passthroughToShard with shard0 finds
        // the data where it was written.
        assert.commandWorked(
            mongos.adminCommand({
                enableSharding: dbName,
                primaryShard: st.shard0.shardName,
            }),
        );
    });

    after(() => {
        st.stop();
    });

    beforeEach(() => {
        cst = new ChangeStreamTest(testDB);
    });

    afterEach(() => {
        if (cst) {
            cst.cleanUp();
        }
        cst = null;
    });

    describe("unsharded collection - change stream targets primary shard only", () => {
        const collName = "unsharded_coll";
        let startTime;

        before(() => {
            const coll = testDB.getCollection(collName);
            coll.drop();

            // Record T0 before creating the collection so the change stream's oplog scan covers
            // the createCollection entry and all subsequent inserts.
            startTime = getClusterTime(testDB);

            assert.commandWorked(testDB.createCollection(collName));
            insertDocs(coll);
        });

        after(() => {
            assert(testDB.getCollection(collName).drop());
        });

        for (const [version, options] of Object.entries(changeStreamOptions)) {
            it(`forwards operationResponseMaxMS to the primary shard and returns multiple empty batches - ${version} change stream`, () => {
                runTest(cst, collName, startTime, [st.rs0.getPrimary()], 4, {}, options);
            });
        }
    });

    describe("sharded collection - change stream targets multiple shards", () => {
        const collName = "sharded_coll";
        const ns = dbName + "." + collName;
        let startTime;

        before(() => {
            const coll = testDB.getCollection(collName);
            coll.drop();

            withBalancerEnabled(st.s, () => {
                // Shard the collection on _id with an explicit split so that low _id values land on
                // shard0 and high _id values land on shard1.
                assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {_id: 1}}));

                // Split at _id: 5 so that inserts with _id 0-4 go to shard0 and _id 5-9 go to shard1.
                assert.commandWorked(mongos.adminCommand({split: ns, middle: {_id: 5}}));
                assert.commandWorked(
                    mongos.adminCommand({moveChunk: ns, find: {_id: 5}, to: st.shard1.shardName}),
                );

                // Record T0 after sharding is set up so the change stream starts from this point.
                startTime = getClusterTime(testDB);

                // Insert kNumDocs documents. Documents with _id < 5 go to shard0; those with _id >= 5
                // go to shard1. The matching document (_id: kNumDocs - 1 = 9) lands on shard1, which
                // exercises the query settings forwarding for that shard's cursor.
                insertDocs(coll);
            });
        });

        after(() => {
            assert(testDB.getCollection(collName).drop());
        });

        for (const [version, options] of Object.entries(changeStreamOptions)) {
            it(`forwards operationResponseMaxMS to all shards and returns multiple empty batches - ${version} change stream`, () => {
                // With kNumDocs split evenly across 2 shards and the matching doc on shard1, the two
                // shards scan in lockstep. Each client getMore yields one PBRT advance per shard pair,
                // giving (kNumDocs / 2 - 1) advances before the match. Use 3 as the lower bound to
                // allow some variability while still distinguishing from the "not forwarded" case (0).
                runTest(
                    cst,
                    collName,
                    startTime,
                    [st.rs0.getPrimary(), st.rs1.getPrimary()],
                    3,
                    {},
                    options,
                );
            });
        }
    });

    describe("$_passthroughToShard - change stream targets a specific shard directly", () => {
        // This exercises the kSpecificShardOnly targeting path in cluster_aggregate.cpp, which
        // forwards the command via serializeForPassthrough() -> addQuerySettingsToRequest().
        const collName = "passthrough_coll";
        let startTime;

        before(() => {
            const coll = testDB.getCollection(collName);
            coll.drop();

            // Record T0 before creating the collection so the change stream's oplog scan covers
            // the createCollection entry and all subsequent inserts.
            startTime = getClusterTime(testDB);

            assert.commandWorked(testDB.createCollection(collName));

            insertDocs(coll);
        });

        after(() => {
            assert(testDB.getCollection(collName).drop());
        });

        for (const [version, options] of Object.entries(changeStreamOptions)) {
            it(`forwards operationResponseMaxMS to the targeted shard via $_passthroughToShard - ${version} change stream`, () => {
                // Open the change stream via mongos but force it to target shard0 directly using
                // $_passthroughToShard. This exercises the kSpecificShardOnly code path.
                runTest(
                    cst,
                    collName,
                    startTime,
                    [st.rs0.getPrimary()],
                    4,
                    {
                        $_passthroughToShard: {shard: st.shard0.shardName},
                    },
                    {},
                    options,
                );
            });
        }
    });

    describe("dynamic shard addition - new cursor receives operationResponseMaxMS", () => {
        // This exercises createUpdatedCommandForNewShard(), which is called by
        // ChangeStreamHandleTopologyChangeStage when a newShardDetected (migrateChunkToNewShard)
        // event causes the change stream to open a cursor on a shard that was not targeted at
        // stream-open time.
        const collName = "dynamic_shard_coll";
        const ns = dbName + "." + collName;

        let startTime;

        beforeEach(() => {
            const coll = testDB.getCollection(collName);
            coll.drop();

            // Shard the collection with all chunks on shard0. The chunk [5, MaxKey) will be moved
            // to shard1 during the test to trigger newShardDetected.
            assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {_id: 1}}));
        });

        afterEach(() => {
            assert(testDB.getCollection(collName).drop());
        });

        for (const [version, options] of Object.entries(changeStreamOptions)) {
            it(`forwards operationResponseMaxMS to a cursor opened dynamically on a new shard - ${version} change stream`, () => {
                // Capture startTime after sharding so the change stream sees the split, the moveChunk
                // event, and the subsequent insert on shard1.
                startTime = getClusterTime(testDB);

                const shard1Primary = st.rs1.getPrimary();
                // Clear shard1's log and capture the resulting near-zero offset so that the fixed-size
                // log ring buffer cannot evict the expected entries before we read them.
                shard1Primary.adminCommand({clearLog: "global"});
                const shard1LogOffset = checkLog.getGlobalLog(shard1Primary).length;

                // Open the change stream. At this point only shard0 has a chunk, so the initial
                // cursor is established on shard0 only.
                const cursor = cst.startWatchingChanges({
                    pipeline: [
                        {$changeStream: {startAtOperationTime: startTime}},
                        {$match: {"fullDocument._id": kTargetId}},
                    ],
                    collection: collName,
                    aggregateOptions: {cursor: {batchSize: 0}},
                    querySettings: {queryKnobs: {operationResponseMaxMS: NumberLong(kScanMaxMS)}},
                });

                // Split [MinKey, MaxKey) at _id: 5 and move the upper half to shard1. This generates
                // a newShardDetected (migrateChunkToNewShard) event that the change stream processes on
                // the next getMore, causing it to call createUpdatedCommandForNewShard() and open a
                // cursor on shard1 with the operationResponseMaxMS knob forwarded.
                assert.commandWorked(mongos.adminCommand({split: ns, middle: {_id: 5}}));
                assert.commandWorked(
                    mongos.adminCommand({
                        moveChunk: ns,
                        find: {_id: kTargetId},
                        to: st.shard1.shardName,
                    }),
                );

                // Insert the target document on shard1 so the dynamically opened cursor has an event
                // to deliver.
                assert.commandWorked(testDB.getCollection(collName).insert({_id: kTargetId}));

                // Enable the failpoint on both shards and drive getMores. When the change stream
                // processes the newShardDetected event it opens the shard1 cursor; the failpoint then
                // causes every doWork() on shard1 to exceed the operationResponseMaxMS deadline,
                // emitting log kResponseDeadlineLogId and confirming the knob was forwarded.
                runWithFailPoint(() => {
                    let foundDoc = null;
                    for (let i = 0; i < 200 && foundDoc === null; i++) {
                        cst.getNextBatch(cursor);
                        const batch = cursor.nextBatch;
                        for (const doc of batch) {
                            if (doc.fullDocument && doc.fullDocument._id === kTargetId) {
                                foundDoc = doc;
                            }
                        }
                    }
                    assert(
                        foundDoc !== null,
                        "Did not receive the document inserted on the dynamically added shard",
                        {kTargetId},
                    );
                });

                // Confirm that log kResponseDeadlineLogId was emitted on shard1, proving that the
                // operationResponseMaxMS knob was forwarded to the dynamically opened cursor.
                assert.soon(
                    () =>
                        checkLog
                            .getGlobalLog(shard1Primary)
                            .slice(shard1LogOffset)
                            .some((log) => {
                                try {
                                    return JSON.parse(log).id === kResponseDeadlineLogId;
                                } catch (e) {
                                    return false;
                                }
                            }),
                    `Expected log ${kResponseDeadlineLogId} on shard1 to confirm that ` +
                        "operationResponseMaxMS was forwarded to the dynamically opened cursor",
                );
            });
        }
    });
});
