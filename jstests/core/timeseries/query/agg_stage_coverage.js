/**
 * Tests that all aggregation stages are either explicitly excluded or can gracefully handle timeseries collections.
 * All aggregation stages are required to appear in this file. There are instructions below on how to add a test case
 * for a new document source.
 *
 * @tags: [
 *   # $listMqlEntities cannot be wrapped in a $facet stage.
 *   do_not_wrap_aggregations_in_facets,
 *   requires_timeseries,
 *   requires_getmore,
 *   requires_fcv_83,
 *   # TODO SERVER-113572 remove this tag once '$_internalComputeGeoNearDistance' is fixed.
 *   known_query_shape_computation_problem
 * ]
 */
import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {areViewlessTimeseriesEnabled} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";

const tsColl = db[jsTestName()];
assertDropCollection(db, tsColl.getName());
assert.commandWorked(db.createCollection(tsColl.getName(), {timeseries: {timeField: "time", metaField: "m"}}));
assert.commandWorked(tsColl.createIndex({"m.loc": "2dsphere"}));
// Insert 10 documents, so the aggregation stages will return some documents to confirm the aggregation stage worked.
const startingTime = new Date();
for (let i = 0; i < 10; i++) {
    assert.commandWorked(
        tsColl.insert({
            _id: i,
            time: new Date(startingTime.getTime() + i * 1000),
            m: {tag: "A", loc: [40, 40]},
            value: i * 10,
        }),
    );
}

// Set up a 2nd collection for stages that need subpipelines.
const otherColl = db[jsTestName() + "_other"];
assertDropCollection(db, otherColl.getName());
for (let i = 0; i < 10; i++) {
    assert.commandWorked(otherColl.insert({_id: i, other: i * 20}));
}

/**
 * Instructions on adding a test case to this file.
 *
 * All aggregation stages **must** appear in one of the following lists:
 * 1. errorTests: Stages that should always error when run on timeseries collections.
 *    These stages should set the StageConstraint 'canRunOnTimeseries' to false.
 * 2. noUnpackTests: Stages that can run on timeseries collections without unpacking the underlying raw buckets.
 *    These stages should set the StageConstraint 'consumesLogicalCollectionData' to false.
 * 3. unpackTests: Stages that require unpacking the underlying raw buckets to run on timeseries collections.
 *    This is expected for all stages that return **user** inserted documents.
 * 4. skippedStages: These stages **must** either be
 *    (a) run on the admin db
 *    (b) stub stages for testing
 *    (c) tested elsewhere
 *    (d) stages that can only run in stream processors
 *    (e) internal only stages (cannot be made by user requests) that run on oplog data
 *
 * Once you've determined which set your new aggregation stage belongs to add a test case to the appropriate set.
 * Each set has a slightly different test format.
 */

// These are stages that will error when run on a timeseries collection.
const errorTests = [
    {
        stage: "$_internalSearchIdLookup",
        pipeline: [{$_internalSearchIdLookup: {}}],
        expectedErrorCodes: [
            10557302, // check for 'canRunOnTimeseries' failed.
        ],
    },
    {
        stage: "$_analyzeShardKeyReadWriteDistribution",
        pipeline: [
            {
                $_analyzeShardKeyReadWriteDistribution: {
                    key: {time: 1},
                    splitPointsAfterClusterTime: new Timestamp(100, 1),
                    splitPointsFilter: {"_id.analyzeShardKeyId": UUID()},
                },
            },
        ],
        expectedErrorCodes: [
            ErrorCodes.IllegalOperation, // cannot run on a timeseries collection
            40324, // fails to run on mongod
        ],
    },
    {
        stage: "$rankFusion",
        pipeline: [
            {
                $rankFusion: {
                    input: {
                        pipelines: {
                            a: [{$sort: {x: -1}}],
                            b: [{$sort: {x: 1}}],
                        },
                    },
                },
            },
        ],
        expectedErrorCodes: [
            10557301, // hybrid search not supported for timeseries on mongod.
            10557300, // hybrid search not supported for timeseries on mongos.
            10170100, // hybrid search must be the first stage of the pipeline.
        ],
    },
    {
        stage: "$scoreFusion",
        pipeline: [
            {
                $scoreFusion: {
                    input: {
                        pipelines: {
                            single: [{$score: {score: "$single", normalization: "minMaxScaler"}}],
                            double: [{$score: {score: "$double", normalization: "none"}}],
                        },
                        normalization: "none",
                    },
                    combination: {method: "avg"},
                },
            },
        ],
        expectedErrorCodes: [
            10557301, // hybrid search not supported for timeseries on mongod.
            10557300, // hybrid search not supported for timeseries on mongos.
            10170100, // hybrid search must be the first stage of the pipeline.
        ],
    },
];

// TODO SERVER-101599 remove 10170100 once 9.0 becomes lastLTS, and timeseries collections
// will not have views anymore.
errorTests.forEach((test) => {
    assert.commandFailedWithCode(
        tsColl.runCommand("aggregate", {pipeline: test.pipeline, cursor: {}}),
        test.expectedErrorCodes,
        test.stage + " expected to fail on timeseries collections.",
    );
});

// These stages should work on timeseries collections without unpacking the underlying raw buckets.
const noUnpackTests = [
    {
        stage: "$_internalApplyOplogUpdate",
        pipeline: [{$_internalApplyOplogUpdate: {oplogUpdate: {$v: 2, diff: {i: {a: Timestamp(0, 0)}}}}}],
        returnsBucketDocs: true,
    },
    {
        stage: "$_internalConvertBucketIndexStats",
        pipeline: [{$_internalConvertBucketIndexStats: {timeField: "time", metaField: "m"}}],
        returnsBucketDocs: true,
    },
    {
        stage: "$_internalUnpackBucket",
        pipeline: [
            {$_internalUnpackBucket: {timeField: "time", metaField: "m", bucketMaxSpanSeconds: NumberInt(3600)}},
        ],
        // Viewful timeseries always appends '$_internalUnpackBucket' stage, which causes an error since the stage
        // can only appear once in a pipeline.
        skipTest: !areViewlessTimeseriesEnabled(db),
    },
    // There are known bugs where some of these stages do not work with viewful timeseries.
    {stage: "$listCatalog", pipeline: [{$listCatalog: {}}], skipTest: !areViewlessTimeseriesEnabled(db)},
    {stage: "$collStats", pipeline: [{$collStats: {latencyStats: {}}}]},
    {stage: "$indexStats", pipeline: [{$indexStats: {}}]},
    {
        stage: "$planCacheStats",
        pipeline: [{$planCacheStats: {}}],
        skipTest: !areViewlessTimeseriesEnabled(db),
        zeroDocsReturned: true,
    },
    {
        stage: "$_unpackBucket",
        pipeline: [{$_unpackBucket: {timeField: "time", metaField: "m"}}],
        skipTest: !areViewlessTimeseriesEnabled(db),
    },
];

const unpackTests = [
    {
        stage: "$_internalBoundedSort",
        // Adding $skip because $_internalBoundedSort cannot be the first stage on the merger when running in a sharded cluster.
        pipeline: [{$skip: 1}, {$_internalBoundedSort: {sortKey: {time: 1}, bound: {base: "min"}}}],
    },
    {stage: "$_internalInhibitOptimization", pipeline: [{$_internalInhibitOptimization: {}}]},
    {
        stage: "$_internalSetWindowFields:",
        pipeline: [{$_internalSetWindowFields: {"output": {"val": {"$locf": "$val"}}}}],
    },
    {stage: "$_internalShredDocuments", pipeline: [{$_internalShredDocuments: {}}]},
    {
        stage: "$_internalStreamingGroup",
        pipeline: [{$_internalStreamingGroup: {_id: "$m", value: {$last: "$time"}, $monotonicIdFields: ["_id"]}}],
    },
    {stage: "$_internalSplitPipeline", pipeline: [{$_internalSplitPipeline: {mergeType: "anyShard"}}]},
    {
        stage: "$_internalComputeGeoNearDistance",
        pipeline: [
            {
                $_internalComputeGeoNearDistance: {
                    key: "m.loc",
                    distanceField: "dist",
                    distanceMultiplier: 1,
                    near: {type: "Point", coordinates: [106.65589, 10.787627]},
                },
            },
        ],
    },
    {
        stage: "$_internalSetWindowFields",
        pipeline: [{$_internalSetWindowFields: {sortBy: {time: 1}, output: {sum: {$sum: {}}}}}],
    },
    {stage: "$addFields", pipeline: [{$addFields: {newField: "$value"}}]},
    {stage: "$bucket", pipeline: [{$bucket: {groupBy: "$value", boundaries: [0, 50, 100]}}]},
    {stage: "$bucketAuto", pipeline: [{$bucketAuto: {groupBy: "$value", buckets: 2}}]},
    {stage: "$count", pipeline: [{$count: "total"}]},
    {stage: "$densify", pipeline: [{$densify: {field: "time", range: {step: 1, unit: "millisecond", bounds: "full"}}}]},
    {stage: "$facet", pipeline: [{$facet: {pipeline1: [{$match: {value: {$gt: 0}}}], pipeline2: [{$limit: 5}]}}]},
    {stage: "$fill", pipeline: [{$fill: {sortBy: {time: 1}, output: {value: {method: "linear"}}}}]},
    {
        stage: "$geoNear",
        pipeline: [
            {
                $geoNear: {
                    near: {type: "Point", coordinates: [106.65589, 10.787627]},
                    key: "m.loc",
                    distanceField: "m.distance",
                },
            },
        ],
    },
    {
        stage: "$graphLookup",
        pipeline: [
            {
                $graphLookup: {
                    from: "other",
                    startWith: "$_id",
                    connectFromField: "_id",
                    connectToField: "parent",
                    as: "graph",
                },
            },
        ],
    },
    {stage: "$group", pipeline: [{$group: {_id: "$m.tag", total: {$sum: "$value"}}}]},
    {stage: "$limit", pipeline: [{$limit: 5}]},
    {stage: "$lookup", pipeline: [{$lookup: {from: "other", localField: "_id", foreignField: "_id", as: "joined"}}]},
    {stage: "$match", pipeline: [{$match: {value: {$gt: 20}}}]},
    {stage: "$merge", pipeline: [{$merge: {into: "outputCollection"}}], zeroDocsReturned: true},
    {stage: "$out", pipeline: [{$out: "outputCollection"}], zeroDocsReturned: true},
    {stage: "$project", pipeline: [{$project: {time: 1, value: 1}}]},
    {stage: "$redact", pipeline: [{$redact: {$cond: {if: {$gt: ["$value", 50]}, then: "$$DESCEND", else: "$$PRUNE"}}}]},
    {stage: "$replaceRoot", pipeline: [{$replaceRoot: {newRoot: "$m"}}]},
    {stage: "$replaceWith", pipeline: [{$replaceWith: "$m"}]},
    {stage: "$sample", pipeline: [{$sample: {size: 1}}]},
    {stage: "$score", pipeline: [{$score: {score: 10}}]},
    {stage: "$set", pipeline: [{$set: {newField: "$value"}}]},
    {stage: "$setWindowFields", pipeline: [{$setWindowFields: {sortBy: {time: 1}, output: {rank: {$rank: {}}}}}]},
    {stage: "$skip", pipeline: [{$skip: 1}]},
    {stage: "$sortByCount", pipeline: [{$sortByCount: "$m.tag"}]},
    {stage: "$sort", pipeline: [{$sort: {time: 1}}]},
    {stage: "$unset", pipeline: [{$unset: "value"}]},
    {stage: "$unwind", pipeline: [{$unwind: {path: "$tags", preserveNullAndEmptyArrays: true}}]},
    {stage: "$unionWith", pipeline: [{$unionWith: {coll: "other"}}]},
];

[...noUnpackTests, ...unpackTests].forEach((test) => {
    if (test.skipTest) {
        jsTest.log.info("Skipping " + test.stage + " test on timeseries collections.");
        return;
    }
    const result = tsColl.aggregate(test.pipeline).toArray();
    if (test.zeroDocsReturned) {
        assert.eq(result.length, 0, test.stage + " expected to return zero documents on timeseries collections.");
        return;
    }
    assert(result.length > 0, test.stage + " expected to return documents on timeseries collections.");
    if (!test.returnsBucketDocs) {
        // Confirm the documents were not bucket documents. We will just look at the first document
        // and ensure there is no "control.min.time" field which all bucket documents have.
        assert(
            !result[0].hasOwnProperty("control"),
            test.stage + " expected to not return bucket documents on timeseries collections.",
        );
    }
});

// The following pipeline stages do not need to be tested for timeseries collections.
// Stages that are skipped **must** be one of the following:
// 1. Stages that only run on the admin database.
// 2. Stub stages that are defined for tests in aggregation_stage_stub_parsers.json.
// 3. Stages that are tested elsewhere.
// 4. Stages that can only run in stream processors.
// 5. Stages that cannot be made by user requests and run on oplog data.
const skippedStages = [
    // All change stream stages are temporarily here. TODO SERVER-113494 enable tests here.
    "$changeStream",
    "$changeStreamSplitLargeEvent",
    "$_internalChangeStreamAddPostImage",
    "$_internalChangeStreamAddPreImage",
    "$_internalChangeStreamCheckInvalidate",
    "$_internalChangeStreamCheckResumability",
    "$_internalChangeStreamCheckTopologyChange",
    "$_internalChangeStreamHandleTopologyChange",
    "$_internalChangeStreamInjectControlEvents",
    "$_internalChangeStreamOplogMatch",
    "$_internalChangeStreamTransform",
    "$_internalChangeStreamUnwindTransaction",

    // Stages on the admin DB or run with aggregate: 1.
    "$listMqlEntities",
    "$documents",
    "$currentOp",
    "$listClusterCatalog",
    "$listSampledQueries",
    "$shardedDataDistribution",
    "$querySettings",
    "$listLocalSessions",
    "$listSessions",
    "$_backupFile",
    "$listExtensions",
    "$backupCursor",
    "$backupCursorExtend",
    "$listCachedAndActiveUsers",
    "$queryStats",
    "$_internalShardServerInfo",
    "$_internalListCollections",
    "$_internalAllCollectionStats",
    "$queue",

    // Stub stages defined in 'aggregation_stage_stub_parsers.json'.
    "$stubStage",
    "$testFoo",

    // Stages tested in 'search_disallowed_on_timeseries.js', since they require extra setup.
    "$search",
    "$searchMeta",
    "$vectorSearch",
    "$searchBeta",
    "$listSearchIndexes",
    "$setVariableFromSubPipeline",

    // Stages tested in 'agg_stage_coverage_internal_client.js', since they require extra setup.
    "$mergeCursors",
    "$_internalDensify",
    "$setMetadata",

    // Stages that can only run in stream processors.
    "$hoppingWindow",
    "$tumblingWindow",
    "$sessionWindow",
    "$validate",
    "$setStreamMeta",
    "$https",
    "$cachedLookup",
    "$externalFunction",

    // Stages that cannot be made by user requests and run on oplog data.
    "$_internalFindAndModifyImageLookup",
    "$_internalReshardingIterateTransaction",
    "$_internalReshardingOwnershipMatch",
    "$_addReshardingResumeId",
];

const testedStages = [...errorTests, ...noUnpackTests, ...unpackTests].map((test) => test.stage);

// Use $listMqlEntities to confirm that all aggregation stages have been tested with timeseries collection or skipped.
const aggStages = db
    .getSiblingDB("admin")
    .aggregate([{$listMqlEntities: {entityType: "aggregationStages"}}])
    .toArray()
    .map((obj) => obj.name);

for (const aggStage of aggStages) {
    // Confirm that every aggregation stage is either tested or explicitly skipped.
    if (testedStages.includes(aggStage) || skippedStages.includes(aggStage)) {
        continue;
    }
    assert(false, aggStage + " has not been tested with timeseries collections.");
}
