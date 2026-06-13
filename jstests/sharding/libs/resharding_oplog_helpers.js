/**
 * Shared helpers for resharding oplog tests and their timeseries variants.
 *
 * Exported functions:
 *   - runOplogFetcherReplLagTest(config)
 *   - runOplogSyncAggAssertMinOplogTest(config)
 *   - runOplogSyncAggResumeTokenTest(config)
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {restartServerReplication, stopServerReplication} from "jstests/libs/write_concern_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {skipTestIfSizeBasedOplogTruncationDisabled} from "jstests/libs/oplog_truncation_util.js";

// ---------------------------------------------------------------------------
// Pair 1: resharding_oplog_fetcher_repl_lag
// ---------------------------------------------------------------------------

/**
 * Returns profiler entries for the resharding oplog fetcher aggregate command.
 */
function getProfilerEntries(conn) {
    return conn
        .getDB("local")
        .system.profile.find({
            op: "command",
            ns: "local.oplog.rs",
            "command.aggregate": "oplog.rs",
        })
        .toArray()
        .filter((entry) => {
            for (let stage of entry.command.pipeline) {
                if (stage.hasOwnProperty("$_internalReshardingIterateTransaction")) {
                    return true;
                }
            }
            return false;
        });
}

function resetProfilerCollection(db) {
    db.setProfilingLevel(0);
    assert(db.system.profile.drop());
    db.createCollection("system.profile", {capped: true, size: 100 * 1024 * 1024});
    db.setProfilingLevel(2);
}

/**
 * Returns the read preference used by the command with the given profiler entry.
 */
function getReadPreference(profilerEntry) {
    assert(profilerEntry.hasOwnProperty("command"), profilerEntry);
    assert(profilerEntry.command.hasOwnProperty("$queryOptions"), profilerEntry);
    assert(profilerEntry.command["$queryOptions"].hasOwnProperty("$readPreference"), profilerEntry);
    return profilerEntry.command["$queryOptions"]["$readPreference"].mode;
}

/**
 * Runs the resharding oplog fetcher replication-lag test.
 *
 * @param {Object} config
 * @param {function(st, dbName, collName): void} config.setupCollection
 *   Callback that creates the collection and inserts initial documents.
 *   Receives the ShardingTest instance, dbName, and collName.
 */
export function runOplogFetcherReplLagTest(config) {
    const {setupCollection} = config;

    const st = new ShardingTest({
        shards: 2,
        rs: {
            nodes: 3,
            // Disallow chaining to force both secondaries to sync from the primary. This disables
            // replication on one of the secondaries, with chaining that would effectively disable
            // replication on both secondaries, causing the test setup to be wrong since writeConcern
            // of w: majority is unsatisfiable.
            settings: {chainingAllowed: false},
        },
    });

    const dbName = "testDb";
    const collName = "testColl";
    const ns = dbName + "." + collName;

    assert.commandWorked(
        st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
    );

    setupCollection(st, dbName, collName);

    const configPrimary = st.configRS.getPrimary();
    const donorPrimary = st.rs0.getPrimary();

    function runMoveCollection(host, ns, toShard) {
        const mongos = new Mongo(host);
        return mongos.adminCommand({moveCollection: ns, toShard});
    }

    // Turn on profiling on all nodes on the donor shard.
    st.rs0.nodes.forEach((node) => resetProfilerCollection(node.getDB("local")));

    const beforeQueryingRemainingTimeFp = configureFailPoint(
        configPrimary,
        "hangBeforeQueryingRecipients",
    );
    const beforeCriticalSectionFp = configureFailPoint(
        configPrimary,
        "reshardingPauseCoordinatorBeforeBlockingWrites",
    );
    const moveThread = new Thread(runMoveCollection, st.s.host, ns, st.shard1.shardName);
    moveThread.start();

    beforeQueryingRemainingTimeFp.wait();

    // Verify that the resharding oplog fetcher initially fetches from the nearest node.
    let numEntriesNearestReadPref = 0;
    st.rs0.nodes.forEach((node) => {
        const profilerEntries = getProfilerEntries(node);
        profilerEntries.forEach((profilerEntry) => {
            const readPref = getReadPreference(profilerEntry);
            assert.eq(readPref, "nearest");
            numEntriesNearestReadPref++;
        });
    });
    assert.gt(
        numEntriesNearestReadPref,
        0,
        "Expected to find resharding oplog fetcher profiler entries with read preference 'nearest'",
    );

    beforeQueryingRemainingTimeFp.off();
    beforeCriticalSectionFp.wait();

    // Verify that the resharding oplog fetcher eventually starts fetching from the primary now that
    // the recipient is approaching strict consistency to prepare for the critical section.
    let numEntriesPrimaryReadPrefBefore = 0;
    assert.soon(() => {
        numEntriesPrimaryReadPrefBefore = 0;
        st.rs0.nodes.forEach((node) => {
            const profilerEntries = getProfilerEntries(node);
            if (node == donorPrimary) {
                profilerEntries.forEach((profilerEntry) => {
                    const readPref = getReadPreference(profilerEntry);
                    assert(readPref == "primary" || readPref == "nearest", readPref);
                    if (readPref == "primary") {
                        numEntriesPrimaryReadPrefBefore++;
                    }
                });
            } else {
                profilerEntries.forEach((profilerEntry) => {
                    const readPref = getReadPreference(profilerEntry);
                    assert.eq(readPref, "nearest");
                });
                // Drop the profiler entries on the secondaries so that we can verify later that the
                // resharding oplog fetcher was still fetching from the primary after the critical
                // section had started.
                resetProfilerCollection(node.getDB("local"));
            }
        });
        return numEntriesPrimaryReadPrefBefore > 0;
    }, "Expected to find resharding oplog fetcher profiler entries with read preference 'primary'");

    // Pause replication on one of the secondaries so that if the resharding oplog fetcher targets
    // nearest node instead of the primary at any point after this, the resharding operation could
    // get stuck in the critical section.
    stopServerReplication(st.rs0.getSecondaries()[1]);

    beforeCriticalSectionFp.off();
    assert.commandWorked(moveThread.returnData());

    // Verify that the resharding oplog fetcher was still fetching from the primary after the
    // critical section had started.
    let numEntriesPrimaryReadPrefAfter = 0;
    st.rs0.nodes.forEach((node) => {
        const profilerEntries = getProfilerEntries(node);
        if (node == donorPrimary) {
            profilerEntries.forEach((profilerEntry) => {
                const readPref = getReadPreference(profilerEntry);
                assert(readPref == "primary" || readPref == "nearest", readPref);
                if (readPref == "primary") {
                    numEntriesPrimaryReadPrefAfter++;
                }
            });
        } else {
            assert.eq(profilerEntries.length, 0, {profilerEntries});
        }
    });
    assert.gte(
        numEntriesPrimaryReadPrefAfter,
        numEntriesPrimaryReadPrefBefore,
        "Expected to find at least as many resharding oplog fetcher profiler entries with read " +
            "preference 'primary' before and after the critical section",
    );

    jsTest.log(
        "Profiler entry counts: " +
            tojson({
                numEntriesNearestReadPref,
                numEntriesPrimaryReadPrefBefore,
                numEntriesPrimaryReadPrefAfter,
            }),
    );

    restartServerReplication(st.rs0.getSecondaries()[1]);
    st.stop();
}

// ---------------------------------------------------------------------------
// Pair 2: resharding_oplog_sync_agg_assert_min_oplog
// ---------------------------------------------------------------------------

/**
 * Runs the resharding oplog sync agg assert-min-oplog test.
 *
 * @param {Object} config
 * @param {function(testDB, testColl): void} config.setupCollection
 *   Callback that creates the collection if necessary (e.g. with timeseries options).
 *   The collection object is pre-created as testDB.foo before this is called; for the plain
 *   variant this can be a no-op.
 * @param {function(localDb, testColl, lastBeforeTs: Timestamp|null): Object} config.findAnchorOplogEntry
 *   Callback that returns the oplog entry to use as the anchor for the $gte pipeline.
 *   For the non-TS variant: finds the entry by {op: "i", "o._id": 0}.
 *   For the TS variant: finds the first bucket entry written after lastBeforeTs.
 *   The third argument is the ts of the last oplog entry before the large inserts (null for
 *   non-TS, where the anchor is found by _id instead).
 * @param {function(testColl, counter: number): void} config.insertNextDoc
 *   Callback to insert the next document in the fill-oplog loop.
 *   For non-TS: insert {_id: counter, longString}.
 *   For TS: insert {ts: new Date(), sensorId: {x: counter}, longString}.
 */
export function runOplogSyncAggAssertMinOplogTest(config) {
    const {setupCollection, findAnchorOplogEntry, insertNextDoc} = config;

    const rst = new ReplSetTest({
        // Set the syncdelay to 1s to speed up checkpointing.
        nodeOptions: {syncdelay: 1},
        nodes: 1,
    });
    // Set max oplog size to 1MB, disable time-based retention
    rst.startSet({oplogSize: 1, oplogMinRetentionHours: 0.000001});
    rst.initiate();

    // This test relies on size-based oplog truncation, which may be disabled in disagg.
    skipTestIfSizeBasedOplogTruncationDisabled(rst.getPrimary(), () => rst.stopSet());

    jsTest.log("Inserting documents to generate oplog entries");
    let testDB = rst.getPrimary().getDB("test");
    let testColl = testDB.foo;
    const localDb = rst.getPrimary().getDB("local");

    setupCollection(testDB, testColl);

    // ~400KB each so that oplog can keep at most two insert oplog entries.
    const longString = "a".repeat(400 * 1024);

    // Record the last oplog entry before the large inserts so the TS variant can anchor by ts.
    const lastBefore = localDb.oplog.rs.find().sort({$natural: -1}).limit(1).next();

    // Fill the oplog with two large documents.
    assert.commandWorked(insertNextDoc(testColl, 0, longString));
    assert.commandWorked(insertNextDoc(testColl, 1, longString));

    let oplogEntry = findAnchorOplogEntry(localDb, testColl, lastBefore.ts);
    assert.neq(null, oplogEntry, "Expected to find an oplog entry after the large inserts");

    jsTest.log("Run aggregation pipeline on oplog with $_requestReshardingResumeToken set");
    assert.commandWorked(
        localDb.runCommand({
            aggregate: "oplog.rs",
            pipeline: [{$match: {ts: {$gte: oplogEntry.ts}}}],
            $_requestReshardingResumeToken: true,
            cursor: {},
        }),
    );

    let counter = 2;
    assert.soon(() => {
        // Keep inserting documents until the oplog truncates past the original entry.
        assert.commandWorked(insertNextDoc(testColl, counter, longString));
        counter++;
        let doc;
        try {
            doc = localDb.oplog.rs.findOne();
        } catch (e) {
            if (e.code == ErrorCodes.CappedPositionLost) {
                // Oplog truncation occurred concurrently with the find command above.
                return false;
            }
            throw e;
        }
        return timestampCmp(doc.ts, oplogEntry.ts) == 1;
    }, "Timeout waiting for oplog to roll over on primary");

    assert.commandFailedWithCode(
        localDb.runCommand({
            aggregate: "oplog.rs",
            pipeline: [{$match: {ts: {$gte: oplogEntry.ts}}}],
            $_requestReshardingResumeToken: true,
            cursor: {},
        }),
        ErrorCodes.OplogQueryMinTsMissing,
    );

    jsTest.log(
        "Run aggregation pipeline on incomplete oplog with $_requestReshardingResumeToken set to false",
    );
    assert.commandWorked(
        localDb.runCommand({
            aggregate: "oplog.rs",
            pipeline: [{$match: {ts: {$gte: oplogEntry.ts}}}],
            $_requestReshardingResumeToken: false,
            cursor: {},
        }),
    );

    jsTest.log("Run non-$gte oplog aggregation pipeline with $_requestReshardingResumeToken set");
    assert.commandFailedWithCode(
        localDb.runCommand({
            aggregate: "oplog.rs",
            pipeline: [{$match: {"op": "i"}}],
            $_requestReshardingResumeToken: true,
            cursor: {},
        }),
        ErrorCodes.InvalidOptions,
    );

    jsTest.log("End of test");

    rst.stopSet();
}

// ---------------------------------------------------------------------------
// Pair 3: resharding_oplog_sync_agg_resume_token
// ---------------------------------------------------------------------------

/**
 * Returns true if timestamp 'ts1' value is greater than timestamp 'ts2' value.
 */
function timestampGreaterThan(ts1, ts2) {
    return (
        ts1.getTime() > ts2.getTime() ||
        (ts1.getTime() == ts2.getTime() && ts1.getInc() > ts2.getInc())
    );
}

/**
 * Runs the resharding oplog sync agg resume-token test.
 *
 * @param {Object} config
 * @param {function(testDB, collName): void} config.setupCollection
 *   Callback that creates the collection (with timeseries options for the TS variant).
 *   For the plain variant this can be a no-op since a plain collection needs no explicit creation.
 * @param {function(i: number): Object} config.makeDocument
 *   Returns the i-th document to insert. Non-TS: {x: i}. TS: {ts: new Date(), sensorId: {x: i}, v: i}.
 * @param {string} config.oplogFilterField
 *   The field path used in the $match pipeline filter. Non-TS: "o.x". TS: "o.meta.x".
 */
export function runOplogSyncAggResumeTokenTest(config) {
    const {setupCollection, makeDocument, oplogFilterField} = config;

    let rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();

    const dbName = "test";
    const collName = "foo";
    const ns = dbName + "." + collName;

    // Insert documents to generate oplog entries.
    let testDB = rst.getPrimary().getDB(dbName);
    let testColl = testDB.foo;

    setupCollection(testDB, collName);

    for (let i = 0; i < 10; i++) {
        assert.commandWorked(testColl.insert(makeDocument(i)));
    }

    const localDb = rst.getPrimary().getDB("local");

    // Run aggregation pipeline on oplog with $_requestReshardingResumeToken set when the pipeline
    // can be optimized away.
    const resEnabled = localDb.runCommand({
        aggregate: "oplog.rs",
        pipeline: [{$match: {ts: {$gte: Timestamp(0, 0)}}}],
        $_requestReshardingResumeToken: true,
        cursor: {batchSize: 1},
    });

    assert.commandWorked(resEnabled);
    assert(resEnabled.cursor.hasOwnProperty("postBatchResumeToken"), resEnabled);
    assert(resEnabled.cursor.postBatchResumeToken.hasOwnProperty("ts"), resEnabled);

    // Ensure that postBatchResumeToken attribute is returned for getMore command.
    const cursorId = resEnabled.cursor.id;
    const resGetMore = assert.commandWorked(
        localDb.runCommand({getMore: cursorId, collection: "oplog.rs"}),
    );

    assert.commandWorked(resGetMore);
    assert(resGetMore.cursor.hasOwnProperty("postBatchResumeToken"), resGetMore);
    assert(resGetMore.cursor.postBatchResumeToken.hasOwnProperty("ts"), resGetMore);

    // Run aggregation pipeline on oplog with $_requestReshardingResumeToken disabled.
    const resDisabled = localDb.runCommand({
        aggregate: "oplog.rs",
        pipeline: [{$match: {ts: {$gte: Timestamp(0, 0)}}}],
        $_requestReshardingResumeToken: false,
        cursor: {},
    });

    assert.commandWorked(resDisabled);
    assert(!resDisabled.cursor.hasOwnProperty("postBatchResumeToken"), resDisabled);

    // Run aggregation pipeline on oplog with $_requestReshardingResumeToken unspecified and
    // defaulting to disabled.
    const resWithout = localDb.runCommand({
        aggregate: "oplog.rs",
        pipeline: [{$match: {ts: {$gte: Timestamp(0, 0)}}}],
        cursor: {},
    });

    assert.commandWorked(resWithout);
    assert(!resWithout.cursor.hasOwnProperty("postBatchResumeToken"), resWithout);

    // Run aggregation pipeline on non-oplog with $_requestReshardingResumeToken set.
    const resNotOplog = localDb.runCommand({
        aggregate: ns,
        pipeline: [{$limit: 100}],
        $_requestReshardingResumeToken: true,
        cursor: {},
    });

    assert.commandFailedWithCode(
        resNotOplog,
        ErrorCodes.FailedToParse,
        "$_requestReshardingResumeToken set on non-oplog should fail",
    );

    // Run $changeStream on oplog with $_requestReshardingResumeToken set.
    const resChangeStreamOnOplogWithRequestReshardingResumeToken = localDb.runCommand({
        aggregate: "oplog.rs",
        pipeline: [{$changeStream: {}}],
        $_requestReshardingResumeToken: true,
        cursor: {},
    });
    assert.commandFailedWithCode(
        resChangeStreamOnOplogWithRequestReshardingResumeToken,
        ErrorCodes.InvalidNamespace,
        "$changeStream on oplog should fail",
    );

    // Run $changeStream with $_requestReshardingResumeToken set on non-oplog collection.
    const resChangeStreamWithRequestReshardingResumeToken = testDB.runCommand({
        aggregate: collName,
        pipeline: [{$changeStream: {}}],
        $_requestReshardingResumeToken: true,
        cursor: {},
    });
    assert.commandFailedWithCode(
        resChangeStreamWithRequestReshardingResumeToken,
        ErrorCodes.FailedToParse,
        "$_requestReshardingResumeToken set with $changeStream should fail",
    );

    // Run aggregation pipeline on oplog with empty batch.
    const resEmpty = localDb.runCommand({
        aggregate: "oplog.rs",
        pipeline: [{$match: {ts: {$gte: Timestamp(0, 0)}}}],
        $_requestReshardingResumeToken: true,
        cursor: {batchSize: 0},
    });

    assert.commandWorked(resEmpty);
    assert(resEmpty.cursor.hasOwnProperty("postBatchResumeToken"), resEmpty);
    assert(resEmpty.cursor.postBatchResumeToken.hasOwnProperty("ts"), resEmpty);
    assert.eq(resEmpty.cursor.postBatchResumeToken.ts, new Timestamp(0, 0));

    // Run aggregation pipeline on oplog with $_requestReshardingResumeToken set when the pipeline
    // can not be optimized away.
    const batchSize = 5;
    let result = localDb.runCommand({
        aggregate: "oplog.rs",
        // The $_internalInhibitOptimization prevents the pipeline from being optimized away as a
        // simple plan executor. This is necessary to force the pipeline to be evaluated using
        // PlanExecutorPipeline.
        pipeline: [
            {$match: {ts: {$gte: Timestamp(0, 0)}, [oplogFilterField]: {$lt: 8}}},
            {$_internalInhibitOptimization: {}},
        ],
        $_requestReshardingResumeToken: true,
        cursor: {batchSize: batchSize},
    });
    assert.commandWorked(result);
    assert(result.cursor.hasOwnProperty("postBatchResumeToken"), result);
    assert(result.cursor.postBatchResumeToken.hasOwnProperty("ts"), result);
    assert.eq(result.cursor.firstBatch.length, batchSize, result);

    // Verify that the postBatchResumeToken is equal to the 'ts' of the last record.
    assert.eq(
        result.cursor.postBatchResumeToken.ts,
        result.cursor.firstBatch[result.cursor.firstBatch.length - 1].ts,
    );

    // Ensure that postBatchResumeToken attribute is returned for getMore command by reading the
    // second batch. There are not enough matching documents left in the oplog to fill an entire
    // batch, so we expect the PBRT to exceed the ts of the final entry.
    result = assert.commandWorked(
        localDb.runCommand({getMore: result.cursor.id, collection: "oplog.rs"}),
    );
    assert(result.cursor.hasOwnProperty("postBatchResumeToken"), result);
    assert(result.cursor.postBatchResumeToken.hasOwnProperty("ts"), result);
    let resultsBatch = result.cursor.nextBatch;
    assert(resultsBatch.length < batchSize, result);

    // Verify that the postBatchResumeToken is greater than the 'ts' of the last read record since
    // the documents in the rest of the collection do not match the filter.
    assert(
        timestampGreaterThan(
            result.cursor.postBatchResumeToken.ts,
            resultsBatch[resultsBatch.length - 1].ts,
        ),
        "postBatchResumeToken value should be greater than 'ts' of the last record",
    );

    // Read all records in one batch.
    result = localDb.runCommand({
        aggregate: "oplog.rs",
        pipeline: [
            {$match: {ts: {$gte: Timestamp(0, 0)}, [oplogFilterField]: {$lt: 2}}},
            {$_internalInhibitOptimization: {}},
        ],
        $_requestReshardingResumeToken: true,
        cursor: {},
    });
    assert.commandWorked(result);
    assert(result.cursor.hasOwnProperty("postBatchResumeToken"), result);
    assert(result.cursor.postBatchResumeToken.hasOwnProperty("ts"), result);
    resultsBatch = result.cursor.firstBatch;

    // Verify that the postBatchResumeToken is greater than the 'ts' of the last read record since
    // the documents in the rest of the collection do not match the filter.
    assert(
        timestampGreaterThan(
            result.cursor.postBatchResumeToken.ts,
            resultsBatch[resultsBatch.length - 1].ts,
        ),
        "postBatchResumeToken value should be greater than 'ts' of the last record",
    );

    // Run aggregation pipeline on oplog with batchSize: 0 when the pipeline can not be optimized
    // away.
    result = localDb.runCommand({
        aggregate: "oplog.rs",
        pipeline: [
            {$match: {ts: {$gte: Timestamp(0, 0)}, [oplogFilterField]: {$lt: 2}}},
            {$_internalInhibitOptimization: {}},
        ],
        $_requestReshardingResumeToken: true,
        cursor: {batchSize: 0},
    });

    assert.commandWorked(result);
    assert(result.cursor.hasOwnProperty("postBatchResumeToken"), result);
    assert(result.cursor.postBatchResumeToken.hasOwnProperty("ts"), result);
    assert.eq(result.cursor.postBatchResumeToken.ts, new Timestamp(0, 0));

    // Run aggregation pipeline on oplog with $_requestReshardingResumeToken set to false when the
    // pipeline can not be optimized away.
    result = localDb.runCommand({
        aggregate: "oplog.rs",
        pipeline: [{$match: {ts: {$gte: Timestamp(0, 0)}}}, {$_internalInhibitOptimization: {}}],
        $_requestReshardingResumeToken: false,
        cursor: {batchSize: 5},
    });
    assert.commandWorked(result);
    assert(!result.cursor.hasOwnProperty("postBatchResumeToken"), result);
    rst.stopSet();
}
