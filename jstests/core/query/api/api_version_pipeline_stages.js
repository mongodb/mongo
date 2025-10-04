/**
 * Tests commands(e.g. aggregate, create) that use pipeline stages not supported in API Version 1.
 *
 * Tests which create views aren't expected to work when collections are implicitly sharded.
 * @tags: [
 *   assumes_read_concern_unchanged,
 *   assumes_read_preference_unchanged,
 *   assumes_unsharded_collection,
 *   uses_api_parameters,
 * ]
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

const testDb = db.getSiblingDB(jsTestName());
const adminDb = db.getSiblingDB("admin");
const collName = "api_version_pipeline_stages";
const coll = testDb[collName];
coll.drop();
coll.insert({a: 1});

const kUnrecognizedPipelineStageErrorCode = 40324;

const unstablePipelines = [
    [{$collStats: {count: {}, latencyStats: {}}}],
    [{$currentOp: {}}],
    [{$indexStats: {}}],
    [{$listLocalSessions: {}}],
    [{$listSessions: {}}],
    [{$planCacheStats: {}}],
    [{$unionWith: {coll: "coll2", pipeline: [{$collStats: {latencyStats: {}}}]}}],
    [{$lookup: {from: "coll2", pipeline: [{$indexStats: {}}]}}],
    [{$facet: {field1: [], field2: [{$indexStats: {}}]}}],
    [{$rankFusion: {input: {pipelines: {field1: [{$sort: {foo: 1}}]}}}}],
    [{$score: {score: 10}}],
    [
        {
            $setWindowFields: {
                sortBy: {_id: 1},
                output: {
                    "relativeXValue": {
                        $minMaxScaler: {
                            input: "$x",
                        },
                        window: {range: ["unbounded", "unbounded"]},
                    },
                },
            },
        },
    ],
    [
        {
            $scoreFusion: {
                input: {
                    pipelines: {
                        score2: [
                            {
                                $search: {index: "search_index", text: {query: "mystery", path: "genres"}},
                            },
                            {$match: {author: "dave"}},
                        ],
                    },
                    normalization: "none",
                },
                combination: {weights: {score2: 5}},
            },
        },
    ],
];

function is81orAbove(db) {
    const res = db.getSiblingDB("admin").system.version.find({_id: "featureCompatibilityVersion"}).toArray();
    return res.length == 0 || MongoRunner.compareBinVersions(res[0].version, "8.1") >= 0;
}

// TODO (SERVER-98651) listClusterCatalog can always be included once backported.
if (is81orAbove(db)) {
    unstablePipelines.push([{$listClusterCatalog: {}}]);
}

function assertAggregateFailsWithAPIStrict(pipeline) {
    // The error code may also be an unrecognized pipeline stage, QueryFeatureNotAllowed, or parsing error, if the test is running in a
    // multiversioned scenario where the stage does not exist or is not enabled on the older version.
    assert.commandFailedWithCode(
        testDb.runCommand({
            aggregate: collName,
            pipeline: pipeline,
            cursor: {},
            apiStrict: true,
            apiVersion: "1",
        }),
        [
            ErrorCodes.APIStrictError,
            kUnrecognizedPipelineStageErrorCode,
            ErrorCodes.QueryFeatureNotAllowed,
            ErrorCodes.FailedToParse,
        ],
    );
}

for (let pipeline of unstablePipelines) {
    // Assert error thrown when running a pipeline with stages not in API Version 1.
    assertAggregateFailsWithAPIStrict(pipeline);

    // Assert error thrown when creating a view on a pipeline with stages not in API Version 1.
    // The error code may also be an unrecognized pipeline stage, QueryFeatureNotAllowed, or parsing error, if the test is running in a
    // multiversioned scenario where the stage does not exist or is not enabled on the older version.
    assert.commandFailedWithCode(
        testDb.runCommand({
            create: "api_version_pipeline_stages_should_fail",
            viewOn: collName,
            pipeline: pipeline,
            apiStrict: true,
            apiVersion: "1",
        }),
        [
            ErrorCodes.APIStrictError,
            kUnrecognizedPipelineStageErrorCode,
            ErrorCodes.QueryFeatureNotAllowed,
            ErrorCodes.FailedToParse,
        ],
    );
}

// Test that $collStats is allowed in APIVersion 1, even with 'apiStrict: true', so long as the only
// parameter given is 'count'.
assertAggregateFailsWithAPIStrict([{$collStats: {latencyStats: {}}}]);
assertAggregateFailsWithAPIStrict([{$collStats: {latencyStats: {histograms: true}}}]);
assertAggregateFailsWithAPIStrict([{$collStats: {storageStats: {}}}]);
assertAggregateFailsWithAPIStrict([{$collStats: {queryExecStats: {}}}]);
assertAggregateFailsWithAPIStrict([{$collStats: {latencyStats: {}, queryExecStats: {}}}]);
assertAggregateFailsWithAPIStrict([{$collStats: {latencyStats: {}, storageStats: {scale: 1024}, queryExecStats: {}}}]);

assert.commandWorked(
    testDb.runCommand({
        aggregate: collName,
        pipeline: [{$collStats: {}}],
        cursor: {},
        apiVersion: "1",
        apiStrict: true,
    }),
);
assert.commandWorked(
    testDb.runCommand({
        aggregate: collName,
        pipeline: [{$collStats: {count: {}}}],
        cursor: {},
        apiVersion: "1",
        apiStrict: true,
    }),
);

// Test that by running the aggregate command with $collStats + $group like our drivers do to
// compute the count, we get back a single result in the first batch - no getMore is required.
// This test is meant to mimic a drivers test and serve as a warning if we may be making a breaking
// change for the drivers.
const cmdResult = assert.commandWorked(
    testDb.runCommand({
        aggregate: collName,
        pipeline: [{$collStats: {count: {}}}, {$group: {_id: 1, count: {$sum: "$count"}}}],
        cursor: {},
        apiVersion: "1",
        apiStrict: true,
    }),
);

assert.eq(cmdResult.cursor.id, 0, cmdResult);
assert.eq(cmdResult.cursor.firstBatch.length, 1, cmdResult);
assert.eq(cmdResult.cursor.firstBatch[0].count, 1, cmdResult);

// TODO(SERVER-106932): $listExtensions can always be included when feature flag is removed.
if (FeatureFlagUtil.isPresentAndEnabled(testDb.getMongo(), "ExtensionsAPI")) {
    // $listExtensions must be run on the adminDB with aggregate: 1, so the
    // command is tested separately from the unstablePipelines array.
    assert.commandFailedWithCode(
        adminDb.runCommand({
            aggregate: 1,
            pipeline: [{$listExtensions: {}}],
            cursor: {},
            apiStrict: true,
            apiVersion: "1",
        }),
        [
            ErrorCodes.APIStrictError,
            kUnrecognizedPipelineStageErrorCode,
            ErrorCodes.QueryFeatureNotAllowed,
            ErrorCodes.FailedToParse,
        ],
    );
}
