/**
 * Tests apply_pipeline_suffix_dependencies for extension source stages, verifies that
 * applyPipelineSuffixDependencies is not invoked on transform stages, and exercises mixed
 * pipelines where both host and extension stages participate in dependency analysis.
 *
 * $trackDepsSource is a source stage that accepts {meta: <name>, var: <name>} and records whether that
 * metadata field / variable is needed by its downstream pipeline, and whether the full document is
 * needed. It emits {_id: 0, neededMeta: <bool>, neededVar: <bool>, neededWholeDoc: <bool>}.
 *
 * $trackDepsTransform is a transform stage that overrides applyPipelineSuffixDependencies and
 * records whether it was invoked. Whether the callback was called is observable via the stage's
 * serialized/explain output: {depsCallbackCalled: <bool>}.
 *
 * $readNDocuments desugars to $produceIds + $_internalSearchIdLookup. $produceIds overrides
 * applyPipelineSuffixDependencies to conditionally produce $score metadata based on whether the
 * suffix references it, and declares providedMetadataFields: ["score"] in its static properties.
 *
 * $addFieldsMatch desugars into host-side $addFields + $match stages at parse time.
 *
 * @tags: [
 *   featureFlagExtensionsAPI,
 *   featureFlagExtensionsOptimizations,
 *   requires_fcv_90,
 * ]
 */
import {getStageFromSplitPipeline} from "jstests/libs/query/analyze_plan.js";
import {
    checkPlatformCompatibleWithExtensions,
    withExtensions,
} from "jstests/noPassthrough/libs/extension_helpers.js";

checkPlatformCompatibleWithExtensions();

/**
 * Runs a pipeline and verifies that the specified fields agree across all shards.
 */
function runPipelineAndCheckShards(coll, pipeline, fields) {
    const results = coll.aggregate(pipeline).toArray();
    const pipelineStr = tojson(pipeline);
    assert.gte(results.length, 1, `Expected at least one result for pipeline: ${pipelineStr}`);
    for (let i = 1; i < results.length; i++) {
        for (const field of fields) {
            assert.eq(
                results[i][field],
                results[0][field],
                `Shard results disagree on ${field} for pipeline: ${pipelineStr}`,
            );
        }
    }
    return results;
}

function assertDeps(coll, downstream, opts) {
    const {
        metaName = "searchSequenceToken",
        varName = "NOW",
        expectedMeta,
        expectedVar,
        expectedWholeDoc,
        expectedNeededFields,
    } = opts;
    const pipeline = [{$trackDepsSource: {meta: metaName, var: varName}}, ...downstream];
    const pipelineStr = tojson(pipeline);
    const results = runPipelineAndCheckShards(coll, pipeline, [
        "neededMeta",
        "neededVar",
        "neededWholeDoc",
        "neededFields",
    ]);
    const {neededMeta, neededVar, neededWholeDoc, neededFields} = results[0];
    assert.eq(neededMeta, expectedMeta, `neededMeta for pipeline: ${pipelineStr}`);
    assert.eq(neededVar, expectedVar, `neededVar for pipeline: ${pipelineStr}`);
    assert.eq(neededWholeDoc, expectedWholeDoc, `neededWholeDoc for pipeline: ${pipelineStr}`);
    if (expectedNeededFields !== undefined) {
        assert.eq(neededFields, expectedNeededFields, `neededFields for pipeline: ${pipelineStr}`);
    }
}

function runSourceTests(coll) {
    // Metadata not referenced downstream, with or without additional stages.
    assertDeps(coll, [], {expectedMeta: false, expectedVar: false, expectedWholeDoc: false});
    assertDeps(coll, [{$limit: 10}, {$project: {neededMeta: 1, neededVar: 1, neededWholeDoc: 1}}], {
        expectedMeta: false,
        expectedVar: false,
        expectedWholeDoc: false,
    });

    // Metadata referenced downstream.
    assertDeps(
        coll,
        [
            {
                $project: {
                    token: {$meta: "searchSequenceToken"},
                    neededMeta: 1,
                    neededVar: 1,
                    neededWholeDoc: 1,
                },
            },
        ],
        {expectedMeta: true, expectedVar: false, expectedWholeDoc: false},
    );
    assertDeps(
        coll,
        [
            {$limit: 100},
            {
                $project: {
                    token: {$meta: "searchSequenceToken"},
                    neededMeta: 1,
                    neededVar: 1,
                    neededWholeDoc: 1,
                },
            },
        ],
        {expectedMeta: true, expectedVar: false, expectedWholeDoc: false},
    );

    // Variable referenced downstream.
    assertDeps(coll, [{$addFields: {timestamp: "$$NOW"}}], {
        expectedMeta: false,
        expectedVar: true,
        expectedWholeDoc: true,
    });
    assertDeps(coll, [{$limit: 100}, {$addFields: {timestamp: "$$NOW"}}], {
        expectedMeta: false,
        expectedVar: true,
        expectedWholeDoc: true,
    });

    // Variable referenced downstream — USER_ROLES.
    assertDeps(coll, [{$addFields: {ct: "$$USER_ROLES"}}], {
        metaName: "searchScore",
        varName: "USER_ROLES",
        expectedMeta: false,
        expectedVar: true,
        expectedWholeDoc: true,
    });

    // Both metadata and variable referenced downstream.
    assertDeps(coll, [{$addFields: {token: {$meta: "searchSequenceToken"}, timestamp: "$$NOW"}}], {
        expectedMeta: true,
        expectedVar: true,
        expectedWholeDoc: true,
    });

    // $addFields implies needsWholeDocument.
    assertDeps(coll, [{$addFields: {score: {$meta: "searchSequenceToken"}}}], {
        expectedMeta: true,
        expectedVar: false,
        expectedWholeDoc: true,
    });

    // needsWholeDocument: inclusive projection does not need the whole document.
    assertDeps(coll, [{$project: {neededMeta: 1, neededVar: 1, neededWholeDoc: 1}}], {
        expectedMeta: false,
        expectedVar: false,
        expectedWholeDoc: false,
    });

    // Variable not referenced downstream — should not be needed.
    assertDeps(coll, [{$addFields: {timestamp: "$$NOW"}}], {
        metaName: "searchScore",
        varName: "USER_ROLES",
        expectedMeta: false,
        expectedVar: false,
        expectedWholeDoc: true,
    });
}

function assertNeededFields(coll, downstream, expectedNeededFields) {
    const pipeline = [{$trackDepsSource: {meta: "searchSequenceToken", var: "NOW"}}, ...downstream];
    const results = runPipelineAndCheckShards(coll, pipeline, ["neededFields"]);
    assert.eq(
        results[0].neededFields,
        expectedNeededFields,
        `neededFields for pipeline: ${tojson(pipeline)}`,
    );
}

function runNeededFieldsTests(coll) {
    // No downstream stages — neededFields should be empty.
    assertNeededFields(coll, [], []);

    // $addFields implies needsWholeDocument — neededFields should be null.
    assertNeededFields(coll, [{$addFields: {extra: 1}}], null);

    // Inclusive projection that includes tracking fields so we can read the result.
    assertNeededFields(
        coll,
        [{$project: {a: 1, b: 1, neededFields: 1, _id: 0}}],
        ["a", "b", "neededFields"],
    );

    // Nested field paths.
    assertNeededFields(
        coll,
        [{$project: {"a.b": 1, "c.d.e": 1, neededFields: 1, _id: 0}}],
        ["a.b", "c.d.e", "neededFields"],
    );

    // $limit followed by inclusive projection — fields flow through $limit.
    assertNeededFields(
        coll,
        [{$limit: 5}, {$project: {foo: 1, neededFields: 1, _id: 0}}],
        ["foo", "neededFields"],
    );

    // Metadata projection with inclusive field projection — fields reported independently.
    assertNeededFields(
        coll,
        [{$project: {score: {$meta: "searchSequenceToken"}, name: 1, neededFields: 1, _id: 0}}],
        ["name", "neededFields"],
    );
}

/**
 * Verify via explain that applyPipelineSuffixDependencies was not invoked on $trackDepsTransform.
 */
function assertTransformDepsNotCalled(coll, pipeline) {
    const explain = coll.explain().aggregate(pipeline);
    const stageObj = getStageFromSplitPipeline(explain, "$trackDepsTransform");
    assert.neq(
        stageObj,
        null,
        `$trackDepsTransform not found in explain output: ${tojson(explain)}`,
    );
    assert.eq(
        stageObj["$trackDepsTransform"].depsCallbackCalled,
        false,
        `applyPipelineSuffixDependencies should not be invoked on transform stages. Explain: ${tojson(explain)}`,
    );
}

function runTransformNegativeTests(coll) {
    // Transform stage alone — callback should not be invoked.
    assertTransformDepsNotCalled(coll, [{$trackDepsTransform: {}}]);

    // Callback should not fire even when downstream stages reference metadata and variables.
    assertTransformDepsNotCalled(coll, [
        {$trackDepsTransform: {}},
        {$addFields: {token: {$meta: "searchSequenceToken"}, timestamp: "$$NOW"}},
    ]);

    // Mixed suffix with both extension transform and host stages. The source stage's suffix
    // deps should reflect the full suffix. The transform should not be invoked.
    {
        const pipeline = [
            {$trackDepsSource: {meta: "searchSequenceToken", var: "NOW"}},
            {$trackDepsTransform: {}},
            {
                $project: {
                    token: {$meta: "searchSequenceToken"},
                    neededMeta: 1,
                    neededVar: 1,
                    neededWholeDoc: 1,
                },
            },
        ];
        assertTransformDepsNotCalled(coll, pipeline);

        const results = coll.aggregate(pipeline).toArray();
        assert.gte(results.length, 1, `Expected at least one result: ${tojson(results)}`);
        assert.eq(
            results[0].neededMeta,
            true,
            `Source stage should detect metadata needed through mixed suffix: ${tojson(results[0])}`,
        );
        assert.eq(
            results[0].neededVar,
            false,
            `No variable reference in mixed suffix: ${tojson(results[0])}`,
        );
        // $trackDepsTransform is in the suffix and all extension stages unconditionally
        // declare needWholeDocument=true in getDependencies.
        assert.eq(
            results[0].neededWholeDoc,
            true,
            `Extension transform in suffix should cause needWholeDocument: ${tojson(results[0])}`,
        );
    }
}

/**
 * Tests where both host and extension stages participate in dependency analysis in the same
 * pipeline.
 */
function runMixedPipelineTests(coll) {
    // Extension source ($readNDocuments/$produceIds) conditionally produces score metadata based
    // on dep analysis. Verify that when the suffix references {$meta: "score"}, the metadata is
    // produced and flows through host stages correctly.
    {
        const results = coll
            .aggregate([
                {$readNDocuments: {numDocs: 1}},
                {$project: {_id: 1, score: {$meta: "score"}}},
            ])
            .toArray();
        assert.eq(results.length, 1, `Expected 1 result, got: ${tojson(results)}`);
        assert.eq(
            results[0].score,
            results[0]._id * 5,
            `Expected score = _id * 5 from $produceIds dep analysis: ${tojson(results[0])}`,
        );
    }

    // Score metadata flows through $limit.
    {
        const results = coll
            .aggregate([
                {$readNDocuments: {numDocs: 1}},
                {$limit: 10},
                {$addFields: {myScore: {$meta: "score"}}},
            ])
            .toArray();
        assert.eq(results.length, 1, `Expected 1 result, got: ${tojson(results)}`);
        assert.eq(
            results[0].myScore,
            results[0]._id * 5,
            `Score metadata should flow through $limit: ${tojson(results[0])}`,
        );
    }

    // When metadata is not referenced, $produceIds should not produce it.
    {
        const results = coll
            .aggregate([{$readNDocuments: {numDocs: 1}}, {$project: {_id: 1, val: 1}}])
            .toArray();
        assert.eq(results.length, 1, `Expected 1 result, got: ${tojson(results)}`);
        assert(
            !results[0].hasOwnProperty("score"),
            `Score should not appear when suffix doesn't reference it: ${tojson(results[0])}`,
        );
    }

    // Extension source ($trackDepsSource) with a suffix that includes a desugared extension stage
    // ($addFieldsMatch). The desugared host stages should participate in dep analysis: $addFields
    // implies needsWholeDocument.
    assertDeps(
        coll,
        [{$addFieldsMatch: {field: "extra", value: 1, filter: {$gt: ["$extra", 0]}}}],
        {
            expectedMeta: false,
            expectedVar: false,
            expectedWholeDoc: true,
        },
    );

    // Desugared $addFieldsMatch in suffix followed by an inclusive $project. The $project
    // sets EXHAUSTIVE_FIELDS so the source only needs the projected fields, not the whole doc.
    assertDeps(
        coll,
        [
            {$addFieldsMatch: {field: "extra", value: 1, filter: {$gt: ["$extra", 0]}}},
            {$project: {neededMeta: 1, neededVar: 1, neededWholeDoc: 1}},
        ],
        {expectedMeta: false, expectedVar: false, expectedWholeDoc: false},
    );

    // Extension source + desugared extension stage + host stage referencing metadata.
    // The $addFieldsMatch desugars into host stages, and a downstream $project references
    // metadata. The inclusive $project limits field deps (needsWholeDoc: false) but the
    // metadata reference is still detected.
    assertDeps(
        coll,
        [
            {$addFieldsMatch: {field: "extra", value: 1, filter: {$gt: ["$extra", 0]}}},
            {
                $project: {
                    token: {$meta: "searchSequenceToken"},
                    neededMeta: 1,
                    neededVar: 1,
                    neededWholeDoc: 1,
                },
            },
        ],
        {expectedMeta: true, expectedVar: false, expectedWholeDoc: false},
    );

    // Extension source + extension transform + desugared extension suffix. Combines all of
    // extension source, extension transform (passthrough), and desugared extension stage expanding
    // to host stages.
    {
        const pipeline = [
            {$trackDepsSource: {meta: "searchSequenceToken", var: "NOW"}},
            {$trackDepsTransform: {}},
            {$addFieldsMatch: {field: "extra", value: 1, filter: {$gt: ["$extra", 0]}}},
            {
                $project: {
                    token: {$meta: "searchSequenceToken"},
                    neededMeta: 1,
                    neededVar: 1,
                    neededWholeDoc: 1,
                },
            },
        ];
        assertTransformDepsNotCalled(coll, pipeline);

        const results = coll.aggregate(pipeline).toArray();
        assert.gte(results.length, 1, `Expected at least one result: ${tojson(results)}`);
        assert.eq(
            results[0].neededMeta,
            true,
            `Source should detect metadata through transform + desugared stages: ${tojson(results[0])}`,
        );
        // $trackDepsTransform (extension stage) unconditionally declares needWholeDocument=true
        // in getDependencies, overriding the inclusive $project at the end.
        assert.eq(
            results[0].neededWholeDoc,
            true,
            `Extension transform in suffix should cause needWholeDocument: ${tojson(results[0])}`,
        );
    }

    // Extension source + extension transform + variable reference through desugared stages.
    assertDeps(
        coll,
        [
            {$trackDepsTransform: {}},
            {$addFieldsMatch: {field: "ts", value: "$$NOW", filter: {$gt: ["$ts", 0]}}},
        ],
        {expectedMeta: false, expectedVar: true, expectedWholeDoc: true},
    );
}

function runAllTestSuites(coll) {
    runSourceTests(coll);
    runNeededFieldsTests(coll);
    runTransformNegativeTests(coll);
    runMixedPipelineTests(coll);
}

function runTests(conn, shardingTest) {
    const db = conn.getDB("test");
    const coll = db[jsTestName()];

    // Insert documents so the collection exists and $readNDocuments can find them.
    assert.commandWorked(
        coll.insertMany(Array.from({length: 10}, (_, i) => ({_id: i, val: i * 10}))),
    );

    // Non-existent metadata type returns an error.
    assert.throwsWithCode(
        () => coll.aggregate([{$trackDepsSource: {meta: "UNKNOWN_META"}}]).toArray(),
        17308,
    );

    // Run on unsharded collection. On mongos this exercises the fromRouter path where the full
    // pipeline is forwarded to a single shard without splitting.
    runAllTestSuites(coll);

    if (shardingTest) {
        assert.commandWorked(db.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));
        assert.commandWorked(db.adminCommand({split: coll.getFullName(), middle: {_id: 5}}));
        assert.commandWorked(
            db.adminCommand({
                moveChunk: coll.getFullName(),
                find: {_id: 5},
                to: shardingTest.shard1.shardName,
            }),
        );

        // Verify the pipeline actually splits via explain.
        {
            const pipeline = [
                {$trackDepsSource: {meta: "searchSequenceToken", var: "NOW"}},
                {
                    $project: {
                        token: {$meta: "searchSequenceToken"},
                        neededMeta: 1,
                        neededVar: 1,
                        neededWholeDoc: 1,
                    },
                },
            ];
            const explain = coll.explain().aggregate(pipeline);
            assert(
                explain.hasOwnProperty("splitPipeline"),
                "Expected splitPipeline in explain output: " + tojson(explain),
            );
        }

        // Re-run on the now-sharded collection, where the pipeline splits across shards.
        runAllTestSuites(coll);
    }
}

withExtensions(
    {
        "libtrack_deps_mongo_extension.so": {},
        "libread_n_documents_mongo_extension.so": {},
        "libadd_fields_match_mongo_extension.so": {},
    },
    runTests,
    ["standalone", "sharded"],
    {shards: 2},
);
