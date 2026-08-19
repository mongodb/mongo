/**
 * Tests dynamic reordering of change stream match expression predicates.
 * @tags: [
 *   uses_change_streams,
 *   does_not_support_stepdowns,
 *   change_stream_does_not_expect_txns,
 * ]
 */
import {withTxnAndAutoRetryOnMongos} from "jstests/libs/auto_retry_transaction_in_sharding.js";
import {
    assertDropAndRecreateCollection,
    assertDropCollection,
} from "jstests/libs/collection_drop_recreate.js";
import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {after, describe, it} from "jstests/libs/mochalite.js";
import {withChangeStreamTest} from "jstests/libs/query/change_stream_util.js";
import {runWithParamsAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

const kParamName = "internalQueryEnableChangeStreamMatchExpressionReordering";

// The same knob is also settable per query shape through query settings, where it is spelled without
// the 'internalQuery' prefix.
const kKnobWireName = "enableChangeStreamMatchExpressionReordering";

const isParamAvailableEverywhere = DiscoverTopology.findNonConfigNodes(db.getMongo()).every(
    (host) =>
        new Mongo(host).getDB("admin").adminCommand({getParameter: 1, [kParamName]: 1}).ok === 1,
);
if (!isParamAvailableEverywhere) {
    jsTest.log.info("Skipping test: parameter is not available on all nodes", {
        parameter: kParamName,
    });
    quit();
}

describe("change stream match expression reordering", () => {
    const collName = jsTestName();
    const otherCollName = `${jsTestName()}_other`;
    const dbName = db.getName();
    const ns = {db: dbName, coll: collName};

    after(() => {
        assertDropCollection(db, collName);
        assertDropCollection(db, otherCollName);
    });

    /**
     * Asserts that 'pipeline' returns 'expectedChanges' for 'workload' both with reordering enabled
     * and with it disabled. Any divergence means reordering changed the semantics of the filter.
     */
    const assertSameEventsRegardlessOfReordering = ({
        pipeline,
        workload,
        expectedChanges,
        doNotModifyInPassthroughs,
    }) => {
        for (const enabled of [true, false]) {
            runWithParamsAllNonConfigNodes(db, {[kParamName]: enabled}, () => {
                const coll = assertDropAndRecreateCollection(db, collName);
                withChangeStreamTest(db, (cst) => {
                    const cursor = cst.startWatchingChanges({
                        pipeline,
                        collection: collName,
                        doNotModifyInPassthroughs,
                    });
                    workload(coll);
                    cst.assertNextChangesEqual({cursor, expectedChanges});
                });
            });
        }
    };

    it("returns identical events for a mixed CRUD workload", () => {
        // Each operation type matches a different branch of the oplog filter's event disjunction, so
        // the per-branch short-circuit counts differ from one another.
        assertSameEventsRegardlessOfReordering({
            pipeline: [{$changeStream: {}}],
            workload: (coll) => {
                assert.commandWorked(coll.insert({_id: 1, a: 1}));
                assert.commandWorked(coll.insert({_id: 2, a: 2}));
                assert.commandWorked(coll.update({_id: 1}, {$set: {a: 10}}));
                assert.commandWorked(coll.update({_id: 2}, {$inc: {a: 5}}));
                // A full-document update produces a "replace" event.
                assert.commandWorked(coll.update({_id: 1}, {_id: 1, b: 1}));
                assert.commandWorked(coll.remove({_id: 2}));
                assert.commandWorked(coll.insert({_id: 3, a: 3}));
                assert.commandWorked(coll.remove({_id: 1}));
            },
            expectedChanges: [
                {operationType: "insert", ns, documentKey: {_id: 1}, fullDocument: {_id: 1, a: 1}},
                {operationType: "insert", ns, documentKey: {_id: 2}, fullDocument: {_id: 2, a: 2}},
                {
                    operationType: "update",
                    ns,
                    documentKey: {_id: 1},
                    updateDescription: {
                        updatedFields: {a: 10},
                        removedFields: [],
                        truncatedArrays: [],
                    },
                },
                {
                    operationType: "update",
                    ns,
                    documentKey: {_id: 2},
                    updateDescription: {
                        updatedFields: {a: 7},
                        removedFields: [],
                        truncatedArrays: [],
                    },
                },
                {operationType: "replace", ns, documentKey: {_id: 1}, fullDocument: {_id: 1, b: 1}},
                {operationType: "delete", ns, documentKey: {_id: 2}},
                {operationType: "insert", ns, documentKey: {_id: 3}, fullDocument: {_id: 3, a: 3}},
                {operationType: "delete", ns, documentKey: {_id: 1}},
            ],
        });
    });

    it("keeps a user-defined $match exact", () => {
        // Predicates from the user's $match are rewritten into the oplog filter by
        // 'DocumentSourceChangeStreamOplogMatch::optimizeAt()', so they are subject to reordering
        // along with the generated predicates. The $or below deliberately mixes a predicate that can
        // be rewritten onto oplog fields with one that cannot.
        assertSameEventsRegardlessOfReordering({
            pipeline: [
                {$changeStream: {}},
                {$match: {$or: [{operationType: "delete"}, {"fullDocument.a": {$gte: 100}}]}},
            ],
            workload: (coll) => {
                assert.commandWorked(coll.insert({_id: 1, a: 1}));
                assert.commandWorked(coll.insert({_id: 2, a: 500}));
                assert.commandWorked(coll.insert({_id: 3, a: 2}));
                assert.commandWorked(coll.remove({_id: 3}));
                assert.commandWorked(coll.insert({_id: 4, a: 700}));
            },
            expectedChanges: [
                {
                    operationType: "insert",
                    ns,
                    documentKey: {_id: 2},
                    fullDocument: {_id: 2, a: 500},
                },
                {operationType: "delete", ns, documentKey: {_id: 3}},
                {
                    operationType: "insert",
                    ns,
                    documentKey: {_id: 4},
                    fullDocument: {_id: 4, a: 700},
                },
            ],
        });
    });

    it("keeps transaction filtering exact", () => {
        // Exercises the second reorderable expression, the filter inside
        // '$_internalChangeStreamUnwindTransaction', which is applied to every operation unwound
        // from an 'applyOps' entry. Writes to another collection in the same transaction must still
        // be filtered out.
        assertDropAndRecreateCollection(db, otherCollName);
        assertSameEventsRegardlessOfReordering({
            doNotModifyInPassthroughs: true,
            pipeline: [{$changeStream: {}}],
            workload: () => {
                const session = db.getMongo().startSession();
                try {
                    const sessionColl = session.getDatabase(dbName)[collName];
                    const sessionOtherColl = session.getDatabase(dbName)[otherCollName];

                    withTxnAndAutoRetryOnMongos(session, () => {
                        assert.commandWorked(sessionColl.insert({_id: 1, a: 1}));
                        // Must not appear in the stream: different collection, same transaction.
                        // Uses a fresh _id because this workload runs twice against a collection
                        // that is not recreated in between.
                        assert.commandWorked(sessionOtherColl.insert({_id: new ObjectId(), a: 99}));
                        assert.commandWorked(sessionColl.insert({_id: 2, a: 2}));
                        assert.commandWorked(sessionColl.update({_id: 1}, {$set: {a: 11}}));
                    });
                } finally {
                    session.endSession();
                }
            },
            expectedChanges: [
                {operationType: "insert", ns, documentKey: {_id: 1}, fullDocument: {_id: 1, a: 1}},
                {operationType: "insert", ns, documentKey: {_id: 2}, fullDocument: {_id: 2, a: 2}},
                {
                    operationType: "update",
                    ns,
                    documentKey: {_id: 1},
                    updateDescription: {
                        updatedFields: {a: 11},
                        removedFields: [],
                        truncatedArrays: [],
                    },
                },
            ],
        });
    });

    it("accepts the knob through query settings", () => {
        // The query settings knob mechanism is feature flagged, so this case is skipped at runtime
        // rather than tagged on the file: tagging would gate the rest of the reordering coverage
        // above on flags it does not need.
        const areQueryKnobsAvailable = ["AllowUserFacingQuerySettings", "PqsQueryKnobs"].every(
            (flag) => FeatureFlagUtil.isPresentAndEnabled(db, flag),
        );
        if (!areQueryKnobsAvailable) {
            jsTest.log.info("Skipping case: query settings knobs are not available");
            return;
        }

        for (const enabled of [true, false]) {
            // A knob that query settings does not recognise is rejected, so the aggregate succeeding
            // here is itself evidence that the knob is settable per query shape. Reordering is a pure
            // optimization, so the events must be identical either way.
            const coll = assertDropAndRecreateCollection(db, collName);
            withChangeStreamTest(db, (cst) => {
                const cursor = cst.startWatchingChanges({
                    pipeline: [{$changeStream: {}}],
                    collection: collName,
                    querySettings: {queryKnobs: {[kKnobWireName]: enabled}},
                });
                assert.commandWorked(coll.insert({_id: 1, a: 1}));
                assert.commandWorked(coll.update({_id: 1}, {$set: {a: 10}}));
                assert.commandWorked(coll.remove({_id: 1}));
                cst.assertNextChangesEqual({
                    cursor,
                    expectedChanges: [
                        {
                            operationType: "insert",
                            ns,
                            documentKey: {_id: 1},
                            fullDocument: {_id: 1, a: 1},
                        },
                        {
                            operationType: "update",
                            ns,
                            documentKey: {_id: 1},
                            updateDescription: {
                                updatedFields: {a: 10},
                                removedFields: [],
                                truncatedArrays: [],
                            },
                        },
                        {operationType: "delete", ns, documentKey: {_id: 1}},
                    ],
                });
            });
        }

        // A non-boolean value for the knob must be rejected.
        assert.commandFailed(
            db.runCommand({
                aggregate: collName,
                pipeline: [{$changeStream: {}}],
                cursor: {},
                querySettings: {queryKnobs: {[kKnobWireName]: "not-a-bool"}},
            }),
        );
    });
});
