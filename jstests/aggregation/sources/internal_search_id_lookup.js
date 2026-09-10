/**
 * Tests `$_internalSearchIdLookup` correctness across clustered collections, collation, and mixed `_id`
 * shapes within a single lookup batch.
 *
 * @tags: [
 *   requires_fcv_90,
 *   assumes_against_mongod_not_mongos,
 *   # The internal-client connection is marked internal by its hello; a read-preference override
 *   # would reroute the internal aggregates to a different, unmarked pooled connection, and the
 *   # server rejects internal-only stages from user connections.
 *   assumes_read_preference_unchanged,
 *   # The failpoint-orchestrated mid-batch its assert exact kill semantics and per-engine
 *   # metrics deltas, which forced replanning and repeated query execution perturb.
 *   does_not_support_repeated_reads,
 *   # Wrapping the mock-mongot pipelines in $facet breaks the failpoint orchestration and the
 *   # result shapes.
 *   do_not_wrap_aggregations_in_facets,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {
    assertDropAndRecreateCollection,
    assertDropCollection,
} from "jstests/libs/collection_drop_recreate.js";
import {ServerStatusMetrics} from "jstests/libs/query/change_stream_metrics_util.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {
    createInternalDB,
    mockMongotPipeline,
    runAggWithMockMongot,
    runAggWithMockMongotResults,
} from "jstests/libs/query/internal_search_id_lookup_util.js";
import {withClusteredColl, withCollation} from "jstests/libs/query/collection_config_decorators.js";

const testDB = db.getSiblingDB(jsTestName());
const internalDB = createInternalDB(testDB.getMongo().uri, testDB.getName());

const configs = [{name: "default", collOpts: {}}]
    .flatMap((config) => [config, withClusteredColl(config)])
    .flatMap((config) => [config, withCollation(config)]);

const oid = ObjectId();
const compound = {a: 1, b: 2};
const docs = [
    {_id: 1, x: "scalar"},
    {_id: "str", x: "string"},
    {_id: oid, x: "oid"},
    {_id: compound, x: "compound"},
    {_id: null, x: "null"},
];

describe("$_internalSearchIdLookup", function () {
    for (const config of configs) {
        describe(config.name, function () {
            const collName = jsTestName() + "_" + config.name.replace(/[^a-zA-Z0-9]/g, "_");
            let coll;

            before(function () {
                coll = assertDropAndRecreateCollection(testDB, collName, config.collOpts);
                assert.writeOK(coll.insert(docs));
            });

            after(function () {
                assertDropCollection(testDB, collName);
            });

            it("finds every `_id` shape via mocked mongot results", function () {
                assert.eq(
                    runAggWithMockMongotResults(
                        internalDB,
                        collName,
                        docs.map((d) => d._id),
                    ),
                    docs,
                );
            });

            // Mixed shapes and outcomes in one batch: no sibling's result may be corrupted, and
            // search.idLookup.<engine> must attribute each outcome to the right engine. Order is
            // asserted exactly too.
            //
            // TODO: SERVER-134080 Support non-scalar _id lookups in SbeSingleDocumentLookupExecutor.
            // Until then, a non-clustered collection's compound _id always declines to the
            // aggregation fallback; the expected counts below reflect that as temporary.
            it("drops missing/absent `_id`s, including non-scalar ones, mixed into the same batch, correctly attributed per engine", function () {
                // Fixed input: 1 (found), "missing-str" (not found), oid (found), {a:9,b:9} (not
                // found, compound), compound (found, compound), null (found), undefined (dropped
                // by enrich() before performLookup, so absent from both cells below). Clustered:
                // SBE handles everything directly. Non-clustered: SBE can't encode compound or
                // null _id, so those entries decline to the aggregation fallback.
                const clustered = config.collOpts.hasOwnProperty("clusteredIndex");
                const expected = clustered
                    ? {
                          sbe: {found: 4, notFound: 2, notHandled: 0},
                          aggregation: {found: 0, notFound: 0},
                      }
                    : {
                          sbe: {found: 2, notFound: 1, notHandled: 3},
                          aggregation: {found: 2, notFound: 1},
                      };

                const delta = ServerStatusMetrics.withServerStatusMetrics(testDB, () => {
                    assert.eq(
                        runAggWithMockMongotResults(internalDB, collName, [
                            1,
                            "missing-str",
                            oid,
                            {a: 9, b: 9},
                            compound,
                            null,
                            undefined,
                        ]),
                        [
                            {_id: 1, x: "scalar"},
                            {_id: oid, x: "oid"},
                            {_id: compound, x: "compound"},
                            {_id: null, x: "null"},
                        ],
                    );
                });

                const sbe = delta.search.idLookup.sbe;
                const aggregation = delta.search.idLookup.aggregation;
                assert.eq(sbe.found, expected.sbe.found, {sbe});
                assert.eq(sbe.notFound, expected.sbe.notFound, {sbe});
                assert.eq(sbe.notHandled, expected.sbe.notHandled, {sbe});
                assert.eq(aggregation.found, expected.aggregation.found, {aggregation});
                assert.eq(aggregation.notFound, expected.aggregation.notFound, {aggregation});
            });

            // Regex/array are the kNotHandled class that trips tassert 13006201 when wired with no
            // fallback. No document can be stored with either type (validIdField rejects both),
            // but mongot's index isn't constrained the same way. Correct outcome: not-found.
            it("declines and correctly reports not-found for regex and array `_id` shapes", function () {
                assert.eq(
                    runAggWithMockMongotResults(internalDB, collName, [/needle/, [1, 2, 3]]),
                    [],
                );
            });

            // Construction-only check: the $limit: 1 carrier runs off the nonexistent namespace
            // and yields nothing, so the idLookup stage never executes a single lookup here —
            // the drop-and-recreate-between-getMores it above covers the missing-ns outcome.
            it("constructs the stage cleanly when the base collection never existed", function () {
                assert.eq(
                    [],
                    runAggWithMockMongotResults(internalDB, "nonexistent_" + collName, [1]),
                );
            });

            it("getMore fails cleanly (no crash) when the collection is dropped and recreated between getMores", function () {
                const recreateCollName = collName + "_recreated";
                const recreateColl = assertDropAndRecreateCollection(
                    testDB,
                    recreateCollName,
                    config.collOpts,
                );
                assert.writeOK(recreateColl.insert({_id: 1, x: "original"}));

                const response = runAggWithMockMongot(internalDB, recreateCollName, [1]);
                assert.eq(response.cursor.firstBatch, [], {response});

                assertDropAndRecreateCollection(testDB, recreateCollName, config.collOpts);
                assert.writeOK(testDB[recreateCollName].insert({_id: 1, x: "recreated"}));

                assert.commandFailedWithCode(
                    internalDB.runCommand({
                        getMore: response.cursor.id,
                        collection: recreateCollName,
                    }),
                    ErrorCodes.QueryPlanKilled,
                );
            });
        });
    }

    // DDL between two enrich() calls of the same plan execution (searchIdLookupMaxBatchSize: 1),
    // unlike the across-getMores case above. A drop still yields the same generic QueryPlanKilled:
    // the whole executor is invalidated before the stage's own checks can run.
    it("getMore fails cleanly (no crash) when the collection is dropped between two enrichment batches of the same getMore", function () {
        const collName = jsTestName() + "_midbatch";
        const coll = assertDropAndRecreateCollection(testDB, collName);
        assert.writeOK(
            coll.insert([
                {_id: 1, x: "first"},
                {_id: 2, x: "second"},
            ]),
        );

        // Let doc 1's batch finish; hang before doc 2's.
        const fp = configureFailPoint(
            testDB.getMongo(),
            "hangBeforeResultsInInternalSearchIdLookup",
            {},
            {skip: 1},
        );

        // funWithArgs re-evaluates this function in a separate shell process, so it can't close
        // over internalDB or mockMongotPipeline; the pipeline is built out here instead and
        // passed in as a plain (serializable) argument.
        //
        // One aggregate command with a real batchSize drives both single-document enrichment
        // batches itself (same as a getMore would), so no cursor needs to cross connections.
        const join = startParallelShell(
            funWithArgs(
                function (dbName, collName, pipeline) {
                    const connInternal = connect(db.getMongo().uri).getMongo();
                    // The server rejects $_internalSearchIdLookup from user connections, so
                    // this fresh connection must mark itself as an internal client in its
                    // first hello before running the aggregate below.
                    assert.commandWorked(
                        connInternal.getDB("admin").runCommand({
                            hello: 1,
                            internalClient: {
                                minWireVersion: NumberInt(0),
                                maxWireVersion: NumberInt(7),
                            },
                        }),
                    );
                    assert.commandFailedWithCode(
                        connInternal.getDB(dbName).runCommand({
                            aggregate: collName,
                            pipeline: pipeline,
                            cursor: {batchSize: 10},
                            readConcern: {},
                            writeConcern: {},
                            querySettings: {queryKnobs: {searchIdLookupMaxBatchSize: 1}},
                        }),
                        ErrorCodes.QueryPlanKilled,
                    );
                },
                testDB.getName(),
                collName,
                mockMongotPipeline([1, 2], {}),
            ),
            testDB.getMongo().port,
        );

        fp.wait();
        try {
            assertDropAndRecreateCollection(testDB, collName);
            assert.writeOK(testDB[collName].insert({_id: 2, x: "recreated"}));
        } finally {
            // Release the hung op before joining: an assert above must not leave the failpoint
            // armed and the parallel shell stuck until the task timeout.
            fp.off();
            join();
        }
    });

    // killOp between two enrich calls of one batch (default searchIdLookupMaxBatchSize 100),
    // while the batch scope and its cached acquisition are still held.
    it("fails cleanly when killed between two enrich calls in the same batch", function () {
        const collName = jsTestName() + "_killop_midbatch";
        const coll = assertDropAndRecreateCollection(testDB, collName);
        assert.writeOK(
            coll.insert([
                {_id: 1, x: "first"},
                {_id: 2, x: "second"},
            ]),
        );

        const comment = "internal_search_id_lookup_killop_midbatch";

        // Skip the first lookup, then hang before the second lookup inside the same batch.
        // 'shouldCheckForInterrupt' makes the hung op observe the killOp and fail with
        // Interrupted; without it the hang only ever ends via fp.off() below, and the op then
        // finishes the tiny remaining batch without necessarily hitting a later interrupt
        // check, so the aggregate can succeed and this test flakes (observed in burn-in).
        const fp = configureFailPoint(
            testDB.getMongo(),
            "hangBeforeResultsInInternalSearchIdLookup",
            {shouldCheckForInterrupt: true},
            {skip: 1},
        );

        const join = startParallelShell(
            funWithArgs(
                function (dbName, collName, pipeline, comment) {
                    const connInternal = connect(db.getMongo().uri).getMongo();
                    assert.commandWorked(
                        connInternal.getDB("admin").runCommand({
                            hello: 1,
                            internalClient: {
                                minWireVersion: NumberInt(0),
                                maxWireVersion: NumberInt(7),
                            },
                        }),
                    );
                    assert.commandFailedWithCode(
                        connInternal.getDB(dbName).runCommand({
                            aggregate: collName,
                            pipeline: pipeline,
                            cursor: {batchSize: 10},
                            readConcern: {},
                            writeConcern: {},
                            comment,
                        }),
                        ErrorCodes.Interrupted,
                    );
                },
                testDB.getName(),
                collName,
                mockMongotPipeline([1, 2], {}),
                comment,
            ),
            testDB.getMongo().port,
        );

        fp.wait();
        try {
            const adminDB = testDB.getMongo().getDB("admin");
            const ops = adminDB
                .aggregate([{$currentOp: {}}, {$match: {"command.comment": comment}}])
                .toArray();
            assert.eq(ops.length, 1, {ops});
            assert.commandWorked(testDB.killOp(ops[0].opid));
        } finally {
            // Release the hung op before joining even if the $currentOp asserts fail above.
            fp.off();
            join();
        }
    });

    // A view-backed search index reapplies the view's pipeline after lookup, forced through the aggregation fallback.
    it("applies the view's defining pipeline to the resolved document, entirely via the aggregation fallback", function () {
        const collName = jsTestName() + "_viewbacked";
        const coll = assertDropAndRecreateCollection(testDB, collName);
        assert.writeOK(
            coll.insert([
                {_id: 1, x: "scalar"},
                {_id: compound, x: "compound"},
            ]),
        );

        const delta = ServerStatusMetrics.withServerStatusMetrics(testDB, () => {
            assert.eq(
                runAggWithMockMongotResults(internalDB, collName, [1, compound], {
                    idLookupSpec: {viewPipeline: [{$addFields: {viewTag: "viewed"}}]},
                }),
                [
                    {_id: 1, x: "scalar", viewTag: "viewed"},
                    {_id: compound, x: "compound", viewTag: "viewed"},
                ],
            );
        });

        const idLookup = (delta.search && delta.search.idLookup) || {};
        const sbe = idLookup.sbe || {};
        const aggregation = idLookup.aggregation || {};
        assert.eq(sbe.found + sbe.notFound + sbe.notHandled, 0, {idLookup});
        assert.eq(aggregation.found, 2, {idLookup});
    });

    // remainingDocumentsToEmit() caps 'limit' by found documents, not documents seen: a window of
    // misses must not stop the pull early. Its own return value (limit minus returned) bounds the
    // next window's pull directly, so no explicit maxBatchSize override is needed to keep windows
    // small near the limit boundary. The metrics assertion below proves ids 4/5/6 never reach the
    // stage, which the result array alone can't distinguish from "pulled everything but truncated
    // the output".
    it("stops after 'limit' found documents despite interleaved misses across single-document windows", function () {
        const collName = jsTestName() + "_limit";
        const coll = assertDropAndRecreateCollection(testDB, collName);
        assert.writeOK(
            coll.insert([
                {_id: 1, x: "found1"},
                {_id: 3, x: "found2"},
                {_id: 5, x: "found3"},
            ]),
        );

        // Alternates present/missing ids so the limit (2) is satisfied by the 1st and 3rd input
        // (id=2's miss in between doesn't count against it), ids 4/5/6 must never be enriched.
        const lookupIds = [1, 2, 3, 4, 5, 6];
        const delta = ServerStatusMetrics.withServerStatusMetrics(testDB, () => {
            assert.eq(
                runAggWithMockMongotResults(internalDB, collName, lookupIds, {
                    idLookupSpec: {limit: 2},
                }),
                [
                    {_id: 1, x: "found1"},
                    {_id: 3, x: "found2"},
                ],
            );
        });

        // Found: ids 1 and 3. Not-found: only id 2's miss; the limit must stop the pull before
        // id 4 (and thus ids 5/6) ever reaches the stage.
        const sbe = (delta.search && delta.search.idLookup && delta.search.idLookup.sbe) || {};
        assert.eq(sbe.found, 2, {sbe});
        assert.eq(sbe.notFound, 1, {sbe});
    });
});
