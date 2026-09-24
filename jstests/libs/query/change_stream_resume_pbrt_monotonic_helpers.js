/**
 * Shared helpers for running change streams PBRT regression tests.
 *
 * Builds a matrix of change-stream resume scenarios across token types and batch sizes and
 * asserts that the resumed stream's initial postBatchResumeToken never regresses below the
 * resume/start token.
 */
import {
    assertCreateCollection,
    assertDropCollection,
} from "jstests/libs/collection_drop_recreate.js";
import {ChangeStreamWatchMode, watchModeToString} from "jstests/libs/query/change_stream_util.js";
import {describe, it} from "jstests/libs/mochalite.js";

export const kBatchSizes = [0, 1, 5];

export const kDataDistributions = Object.freeze({
    oneShard: [
        {_id: -100, shard: 0},
        {_id: -99, shard: 0},
        {_id: -98, shard: 0},
        {_id: -97, shard: 0},
        {_id: -96, shard: 0},
    ],
    bothShards: [
        {_id: -105, shard: 0},
        {_id: -104, shard: 0},
        {_id: 10, shard: 1},
        {_id: 11, shard: 1},
        {_id: 12, shard: 1},
    ],
});

/**
 * Resumes a change stream of the given watch mode from 'resumeToken' using the specified
 * resume option and batch size, then asserts that the initial postBatchResumeToken is not
 * less than the resume/start token. For event resume tokens, also asserts that a token at the
 * same clusterTime was not degraded to a high-water-mark token.
 */
function resumeAndAssertNotRegressed({
    ctx,
    watchMode,
    resumeToken,
    resumeOption,
    batchSize,
    resumeColl,
    expectedRemainingDocs,
}) {
    const changeStreamStage = {[resumeOption]: resumeToken};

    if (watchMode === ChangeStreamWatchMode.kCluster) {
        changeStreamStage.allChangesForCluster = true;
    }

    const collectionArg = watchMode === ChangeStreamWatchMode.kCollection ? resumeColl : 1;
    const resumedCursor = ctx.cst.startWatchingChanges({
        pipeline: [{$changeStream: changeStreamStage}],
        collection: collectionArg,
        aggregateOptions: {cursor: {batchSize}, maxTimeMS: 3000},
    });

    const pbrt = resumedCursor.postBatchResumeToken;
    assert(pbrt, "expected initial postBatchResumeToken", {
        resumedCursor,
        resumeToken,
        batchSize,
        resumeOption,
        watchMode: watchModeToString(watchMode),
    });
    assert.gte(
        bsonWoCompare(pbrt, resumeToken),
        0,
        "postBatchResumeToken regressed below the resume/start token",
        {pbrt, resumeToken, batchSize, resumeOption, watchMode: watchModeToString(watchMode)},
    );

    // An event token must not be degraded to a high-water-mark token at the same clusterTime.
    const decodedResume = decodeResumeToken(resumeToken);
    if (decodedResume.tokenType !== highWaterMarkResumeTokenType) {
        const decodedPbrt = decodeResumeToken(pbrt);
        if (bsonWoCompare(decodedResume.clusterTime, decodedPbrt.clusterTime) === 0) {
            assert.neq(
                decodedPbrt.tokenType,
                highWaterMarkResumeTokenType,
                "PBRT degraded an event token to a high-water-mark token at the same clusterTime",
                {decodedResume, decodedPbrt, resumeToken, pbrt},
            );
        }
    }

    // For event tokens, verify the resumed stream returns the expected documents from after the
    // resume point. When resuming from the last event (expectedRemainingDocs is empty), verify the
    // stream returns zero results.
    if (expectedRemainingDocs !== undefined) {
        const initialBatch = resumedCursor.firstBatch || [];
        if (expectedRemainingDocs.length > 0) {
            const events = ctx.cst.getNextChanges(resumedCursor, expectedRemainingDocs.length);
            assert.eq(
                events.length,
                expectedRemainingDocs.length,
                "resumed stream returned wrong number of events",
                {resumeToken, batchSize, resumeOption, expectedRemainingDocs, events},
            );
            for (let i = 0; i < expectedRemainingDocs.length; i++) {
                assert.eq(
                    events[i].fullDocument,
                    expectedRemainingDocs[i],
                    "resumed stream returned wrong document",
                    {resumeToken, batchSize, i, expected: expectedRemainingDocs[i], events},
                );
            }
        } else {
            // The initial first batch must be empty since there are no events after the token.
            assert.eq(initialBatch.length, 0, "expected empty first batch for last event token", {
                resumeToken,
                batchSize,
                initialBatch,
            });

            // A getMore must also return nothing.
            ctx.cst.getNextBatch(resumedCursor);
            const nextBatch = resumedCursor.nextBatch || resumedCursor.firstBatch || [];
            assert.eq(
                nextBatch.length,
                0,
                "expected empty batch after getMore for last event token",
                {resumeToken, batchSize, nextBatch},
            );
        }
    }
}

function captureEventTokens({ctx, watchMode, distribution}) {
    assertDropCollection(ctx.db, ctx.coll.getName());

    const cursor = ctx.cst.getChangeStream({
        watchMode,
        coll: ctx.coll,
    });

    const docs = kDataDistributions[distribution];
    assert.commandWorked(ctx.coll.insert(docs));

    const events = ctx.cst.getNextChanges(cursor, docs.length);
    assert.eq(events.length, docs.length, "did not consume all inserted events", {events, docs});

    return events.map((event, index) => {
        assert.eq(event.operationType, "insert", "expected an insert event", {event});
        assert(
            !decodeResumeToken(event._id).fromInvalidate,
            "expected a non-invalidate event token",
            {event},
        );
        return {token: event._id, docs, index};
    });
}

function captureHighWaterMarkToken({ctx, watchMode}) {
    const cursor = ctx.cst.getChangeStream({
        watchMode,
        coll: ctx.coll,
    });

    const token = cursor.postBatchResumeToken;
    assert(token, "expected postBatchResumeToken from fresh change stream", {cursor});
    assert.eq(
        decodeResumeToken(token).tokenType,
        highWaterMarkResumeTokenType,
        "expected a high-water-mark resume token",
        {token},
    );
    return token;
}

/**
 * Creates a sharded collection with one chunk on each of two shards. The split point is at {_id: 0},
 * so documents with _id < 0 land on shard0 and _id >= 0 land on shard1.
 */
export function setupShardedCollection({db, coll, st}) {
    assert.commandWorked(
        db.adminCommand({enableSharding: db.getName(), primaryShard: st.shard0.shardName}),
    );
    assertCreateCollection(db, coll.getName());
    assert.commandWorked(db.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));
    assert.commandWorked(db.adminCommand({split: coll.getFullName(), middle: {_id: 0}}));
    assert.commandWorked(
        db.adminCommand({moveChunk: coll.getFullName(), find: {_id: 0}, to: st.shard1.shardName}),
    );
}

/**
 * Resumes a change stream from a captured event token, captures the returned PBRT, then resumes
 * again from that PBRT, asserting that the second PBRT never regresses below the first. This
 * validates the production chaining scenario where a client persists a PBRT and resumes from it.
 */
export function runChainedResumePbrtMonotonicTest({ctx, watchMode}) {
    for (const distribution of Object.keys(kDataDistributions)) {
        it(`chained resume does not regress with ${distribution} data`, function () {
            const tokenInfos = captureEventTokens({ctx, watchMode, distribution});

            // Test the last event token (most regression-prone — no events remain, empty batch).
            const lastTokenInfo = tokenInfos[tokenInfos.length - 1];
            const lastEventToken = lastTokenInfo.token;

            // Verbatim resume from the last event token, batchSize=0 so first batch is empty.
            const changeStreamStage = {resumeAfter: lastEventToken};
            if (watchMode === ChangeStreamWatchMode.kCluster) {
                changeStreamStage.allChangesForCluster = true;
            }
            const collectionArg = watchMode === ChangeStreamWatchMode.kCollection ? ctx.coll : 1;
            const resumedCursor = ctx.cst.startWatchingChanges({
                pipeline: [{$changeStream: changeStreamStage}],
                collection: collectionArg,
                aggregateOptions: {cursor: {batchSize: 0}, maxTimeMS: 3000},
            });

            const pbrt1 = resumedCursor.postBatchResumeToken;
            assert(pbrt1, "expected postBatchResumeToken from resumed stream", {resumedCursor});
            assert.gte(
                bsonWoCompare(pbrt1, lastEventToken),
                0,
                "first PBRT regressed below resume token",
                {pbrt1, lastEventToken},
            );

            // Resume from the PBRT itself (this is what clients do in production).
            const chainedStage = {resumeAfter: pbrt1};
            if (watchMode === ChangeStreamWatchMode.kCluster) {
                chainedStage.allChangesForCluster = true;
            }
            const chainedCursor = ctx.cst.startWatchingChanges({
                pipeline: [{$changeStream: chainedStage}],
                collection: collectionArg,
                aggregateOptions: {cursor: {batchSize: 0}, maxTimeMS: 3000},
            });

            const pbrt2 = chainedCursor.postBatchResumeToken;
            assert(pbrt2, "expected postBatchResumeToken from chained stream", {chainedCursor});
            assert.gte(
                bsonWoCompare(pbrt2, pbrt1),
                0,
                "chained PBRT regressed below the previous PBRT",
                {pbrt2, pbrt1, lastEventToken},
            );

            // Deep chain: resume from the second PBRT as well.
            const deepStage = {resumeAfter: pbrt2};
            if (watchMode === ChangeStreamWatchMode.kCluster) {
                deepStage.allChangesForCluster = true;
            }
            const deepCursor = ctx.cst.startWatchingChanges({
                pipeline: [{$changeStream: deepStage}],
                collection: collectionArg,
                aggregateOptions: {cursor: {batchSize: 0}, maxTimeMS: 3000},
            });

            const pbrt3 = deepCursor.postBatchResumeToken;
            assert.gte(
                bsonWoCompare(pbrt3, pbrt2),
                0,
                "deep-chained PBRT regressed below the previous PBRT",
                {pbrt3, pbrt2, pbrt1},
            );
        });
    }
}

/**
 * Runs the resume PBRT monotonicity matrix for a single change-stream watch mode.
 *
 * Parameters:
 *   - ctx: an object whose 'cst', 'db', and 'coll' properties are populated by the caller's
 *     'before' hook. Using an object lets the helper read the properties at test-run time rather
 *     than capturing undefined values when the helper is invoked at suite-definition time.
 *   - watchMode: one of ChangeStreamWatchMode.kCollection/kDb/kCluster.
 *   - getInvalidateToken (optional): callback({ctx, distribution}) that produces an invalidate
 *     resume token and returns either the token or {token, resumeColl}. If omitted, invalidate
 *     scenarios are skipped (e.g. for whole-cluster streams, which never invalidate).
 */
export function runResumePbrtMonotonicTests({ctx, watchMode, getInvalidateToken}) {
    const tokenTypes = ["event", "highWatermark"];
    if (getInvalidateToken) {
        tokenTypes.push("invalidate");
    }

    for (const tokenType of tokenTypes) {
        describe(`${watchModeToString(watchMode)}-level resume from ${tokenType} token`, function () {
            for (const distribution of Object.keys(kDataDistributions)) {
                it(`does not regress with ${distribution} data across batch sizes ${kBatchSizes.join(
                    ",",
                )} using resumeAfter and startAfter`, function () {
                    let resumeColl = ctx.coll;

                    if (tokenType === "event") {
                        const tokenInfos = captureEventTokens({ctx, watchMode, distribution});
                        for (const {token, docs, index} of tokenInfos) {
                            const expectedRemainingDocs = docs.slice(index + 1);
                            for (const batchSize of kBatchSizes) {
                                for (const resumeOption of ["resumeAfter", "startAfter"]) {
                                    resumeAndAssertNotRegressed({
                                        ctx,
                                        watchMode,
                                        resumeToken: token,
                                        resumeOption,
                                        batchSize,
                                        resumeColl,
                                        expectedRemainingDocs,
                                    });
                                }
                            }
                        }
                        return;
                    }

                    // High-water-mark or invalidate tokens (no document verification).
                    let resumeTokens;
                    const resumeOption =
                        tokenType === "highWatermark" ? "resumeAfter" : "startAfter";
                    if (tokenType === "highWatermark") {
                        resumeTokens = [captureHighWaterMarkToken({ctx, watchMode})];
                    } else {
                        const invalidateResult = getInvalidateToken({ctx, distribution});
                        if (
                            typeof invalidateResult === "object" &&
                            invalidateResult.hasOwnProperty("token")
                        ) {
                            resumeTokens = [invalidateResult.token];
                            if (invalidateResult.hasOwnProperty("resumeColl")) {
                                resumeColl = invalidateResult.resumeColl;
                            }
                        } else {
                            resumeTokens = [invalidateResult];
                        }
                    }

                    for (const resumeToken of resumeTokens) {
                        for (const batchSize of kBatchSizes) {
                            resumeAndAssertNotRegressed({
                                ctx,
                                watchMode,
                                resumeToken,
                                resumeOption,
                                batchSize,
                                resumeColl,
                            });
                        }
                    }
                });
            }
        });
    }
}

/**
 * Runs a control-event scenario for a single change-stream watch mode.
 *
 * The v2 change stream reader swallows internal control events (e.g. namespacePlacementChanged,
 * moveChunk) before DSCSEnsureResumeTokenPresent. Their event resume tokens therefore cannot be
 * used to resume, and must never be exposed as a client-visible postBatchResumeToken (PBRT).
 *
 * The 'changeStreamKeepLastEventResumeTokenAsPBRT' fail point pins each shard's PBRT to the last
 * returned event's resume token, which removes the otherwise racy dependency on whether the shard
 * has already scanned past its last returned event. A placement-changing DDL is then performed and
 * every PBRT delivered to the client is validated by 'ChangeStreamTest' (which rejects swallowed
 * control-event tokens).
 *
 * Parameters:
 *   - ctx: must have 'cst', 'db', and 'coll' populated, and 'ctx.st' set to the ShardingTest.
 *   - watchMode: one of ChangeStreamWatchMode.kCollection/kDb/kCluster.
 */
export function runControlEventPbrtResumableTest({ctx, watchMode}) {
    const kFailPointName = "changeStreamKeepLastEventResumeTokenAsPBRT";
    const kControlOpTypes = ["namespacePlacementChanged", "moveChunk", "movePrimary"];

    describe(`${watchModeToString(watchMode)}-level control-event PBRT leak`, function () {
        it("does not expose a swallowed control event's resume token as PBRT", function () {
            const st = ctx.st;
            const failPointNodes = [st.rs0.getPrimary(), st.rs1.getPrimary()];
            if (st.configRS) {
                failPointNodes.push(st.configRS.getPrimary());
            }

            const setFailPoint = (mode) => {
                for (const node of failPointNodes) {
                    try {
                        node.getDB("admin").runCommand({
                            configureFailPoint: kFailPointName,
                            mode: mode,
                        });
                    } catch (e) {
                        // Ignore cleanup errors (e.g. the primary changed).
                    }
                }
            };

            setFailPoint("alwaysOn");
            try {
                // Earlier scenarios in these tests may have dropped and recreated the watched
                // collection (e.g. the invalidate scenarios). Recreate it unsharded so that
                // 'shardCollection' below emits a namespacePlacementChanged control event.
                assertDropCollection(ctx.db, ctx.coll.getName());
                assertCreateCollection(ctx.db, ctx.coll.getName());
                assert.commandWorked(ctx.coll.createIndex({x: 1}));

                const cursor = ctx.cst.getChangeStream({watchMode, coll: ctx.coll});

                // 'shardCollection' commits placement metadata and emits a namespacePlacementChanged
                // control event for the watched collection.
                assert.commandWorked(
                    ctx.db.adminCommand({
                        shardCollection: ctx.coll.getFullName(),
                        key: {x: 1},
                    }),
                );

                // For a whole-cluster stream, also generate a config-server control event after the
                // shard's control event so that the config cursor's promise is not the minimum.
                if (watchMode === ChangeStreamWatchMode.kCluster) {
                    assert.commandWorked(
                        ctx.db.adminCommand({enableSharding: "controlEventDb_" + jsTestName()}),
                    );
                }

                const observed = [];
                for (let i = 0; i < 40; i++) {
                    const pbrt = cursor.postBatchResumeToken;
                    if (pbrt) {
                        const decoded = decodeResumeToken(pbrt);
                        const opType =
                            decoded.eventIdentifier && decoded.eventIdentifier.operationType;
                        observed.push({
                            clusterTime: decoded.clusterTime,
                            tokenType: decoded.tokenType,
                            operationType: opType,
                        });
                        if (decoded.tokenType !== highWaterMarkResumeTokenType) {
                            assert(
                                !kControlOpTypes.includes(opType),
                                "PBRT is the resume token of an internally swallowed control event",
                                {pbrt, decoded},
                            );
                        }
                    }
                    // 'getNextBatch' validates the batch's PBRT via 'ChangeStreamTest' and advances
                    // the cursor.
                    ctx.cst.getNextBatch(cursor);
                }
                jsTest.log.info("Observed PBRTs in control-event scenario", {
                    watchMode: watchModeToString(watchMode),
                    observed,
                });
            } finally {
                setFailPoint("off");
            }
        });
    });
}
