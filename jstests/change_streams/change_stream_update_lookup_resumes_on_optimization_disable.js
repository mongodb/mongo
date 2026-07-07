/**
 * Tests the optimized change stream updateLookup runtime-disable kill-and-resume behavior: a
 * change stream cursor whose updateLookup stage was wired on the optimized path while
 * featureFlagChangeStreamOptimizedUpdateLookup was on must be throwing a resumable error
 * at its next getMore once the flag is off. The stream then resumes from its token on the
 * legacy aggregation path and keeps returning correct fullDocument values.
 *
 * @tags: [
 *   featureFlagChangeStreamOptimizedUpdateLookup,
 *   requires_fcv_90,
 *   uses_change_streams,
 *   # Raw getMore assertions on a server-killed cursor's exact error code/label would be
 *   # perturbed by transaction-wrapping passthroughs.
 *   change_stream_does_not_expect_txns,
 * ]
 */
import {
    ServerStatusMetrics,
    UpdateLookupExecutor,
} from "jstests/change_streams/change_stream_metrics_util.js";
import {
    assertDropAndRecreateCollection,
    assertDropCollection,
} from "jstests/libs/collection_drop_recreate.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {
    assertGetMoreFailsWithExpectedError,
    withChangeStreamTest,
} from "jstests/libs/query/change_stream_util.js";
import {runWithParamsAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

const flagName = "featureFlagChangeStreamOptimizedUpdateLookup";

describe("change-stream updateLookup resumes on optimization disable", () => {
    const collName = jsTestName();
    let coll;

    before(() => {
        coll = assertDropAndRecreateCollection(db, collName);
        assert.commandWorked(coll.insert({_id: 1, v: 0}));
    });

    after(() => {
        assertDropCollection(db, collName);
    });

    it("retires the optimized cursor and resumes on the legacy path", () => {
        withChangeStreamTest(db, (cst) => {
            // Open the optimized-path stream and confirm it.
            let cursor = cst.startWatchingChanges({
                pipeline: [{$changeStream: {fullDocument: "updateLookup"}}],
                collection: collName,
                aggregateOptions: {cursor: {batchSize: 0}},
            });
            const optimizedCursorId = cursor.id;
            const ns = {db: db.getName(), coll: collName};

            // Ensure the optimized-path read doesn't run the legacy aggregation engine.
            const optimizedReadDelta = ServerStatusMetrics.withServerStatusMetricsAcrossCluster(
                db,
                () => {
                    assert.commandWorked(coll.update({_id: 1}, {$set: {v: 1}}));
                    assert.commandWorked(coll.insert({_id: 2, v: 0}));
                    cst.assertNextChangesEqual({
                        cursor,
                        expectedChanges: [
                            {
                                operationType: "update",
                                ns,
                                documentKey: {_id: 1},
                                fullDocument: {_id: 1, v: 1},
                            },
                            {
                                operationType: "insert",
                                ns,
                                documentKey: {_id: 2},
                                fullDocument: {_id: 2, v: 0},
                            },
                        ],
                    });
                },
            );
            assert.eq(
                optimizedReadDelta.changeStreams.updateLookup[UpdateLookupExecutor.kAggregation]
                    .found,
                0,
                "the aggregation engine ran a lookup while the optimization was on",
                {optimizedReadDelta},
            );

            runWithParamsAllNonConfigNodes(db, {[flagName]: false}, () => {
                assert.commandWorked(coll.update({_id: 1}, {$set: {v: 2}}));

                // Ensure getMore command fails on the existing change stream cursor since IFR flag is turned off.
                assertGetMoreFailsWithExpectedError({
                    db,
                    cursorId: optimizedCursorId,
                    collName,
                    expectedError: (error) => error.code === ErrorCodes.RetryChangeStream,
                    msg: "optimized cursor was not retired with a resumable error after the flag was disabled",
                });

                // Reopen tha change stream, since the flag is off, change stream must be reopened on the
                // legacy path.
                const resumeDelta = ServerStatusMetrics.withServerStatusMetricsAcrossCluster(
                    db,
                    () => {
                        cursor = cst.restartChangeStream(cursor);
                        cst.assertNextChangesEqual({
                            cursor,
                            expectedChanges: [
                                {
                                    operationType: "update",
                                    ns,
                                    documentKey: {_id: 1},
                                    fullDocument: {_id: 1, v: 2},
                                },
                            ],
                        });
                    },
                );

                // Ensure the unoptimized-path read runs the legacy aggregation engine.
                assert.eq(
                    resumeDelta.changeStreams.updateLookup[UpdateLookupExecutor.kAggregation].found,
                    1,
                    "expected exactly one aggregation-engine updateLookup for the resumed stream",
                    {resumeDelta},
                );
            });
        });
    });
});
