/**
 * Verifies multi-updates explains work on sharded timeseries collection. Runs a subset of the test
 * cases included in 'jstests/sharding/timeseries_update_multi.js'.
 *
 * @tags: [
 *   featureFlagTimeseriesUpdatesSupport,
 * ]
 */

import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {
    generateTimeValue,
    makeBucketFilter
} from "jstests/core/timeseries/libs/timeseries_writes_util.js";
import {getExecutionStages} from "jstests/libs/analyze_plan.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {TimeseriesMultiUpdateUtil} from "jstests/sharding/libs/timeseries_update_multi_util.js";

Random.setRandomSeed();

// Connections.
const st = new ShardingTest({shards: 2, rs: {nodes: 2}});
const mongos = st.s0;

const dbName = jsTestName();
const collName = 'sharded_timeseries_update_multi_explain';
const timeField = TimeseriesMultiUpdateUtil.timeField;
const metaField = TimeseriesMultiUpdateUtil.metaField;

// Databases.
assert.commandWorked(mongos.adminCommand({enableSharding: dbName}));
const testDB = mongos.getDB(dbName);
const primaryShard = st.getPrimaryShard(dbName);
const primaryShardName = primaryShard.shardName;
const otherShard = st.getOther(primaryShard);
const otherShardName = otherShard.shardName;

// These configurations contain expected explain results corresponding to a particular shard for
// each update in the update list. Queries including time filters avoid checking bucket filters for
// brevity since they include clustered index optimizations.
const requestConfigurations = {
    // Empty filter leads to broadcasted request.
    emptyFilter: {
        updateList: [{
            q: {},
            u: {$set: {"newField": 1}},
            multi: true,
        }],
        expectedExplain: {
            [primaryShardName]: {
                bucketFilter: makeBucketFilter({}),
                residualFilter: {},
                nBucketsUnpacked: 2,
                nMeasurementsMatched: 2,
                nMeasurementsUpdated: 2,
            },
            [otherShardName]: {
                bucketFilter: makeBucketFilter({}),
                residualFilter: {},
                nBucketsUnpacked: 2,
                nMeasurementsMatched: 2,
                nMeasurementsUpdated: 2,
            }
        }
    },
    // Non-shard key filter without meta or time field leads to broadcasted request.
    nonShardKeyFilter: {
        updateList: [{
            q: {f: 0},
            u: {$unset: {f: ""}},
            multi: true,
        }],
        expectedExplain: {
            [primaryShardName]: {
                bucketFilter: makeBucketFilter({
                    "$and": [
                        {"control.min.f": {"$_internalExprLte": 0}},
                        {"control.max.f": {"$_internalExprGte": 0}}
                    ]
                }),
                residualFilter: {"f": {"$eq": 0}},
                nBucketsUnpacked: 1,
                nMeasurementsMatched: 1,
                nMeasurementsUpdated: 1,
            },
            [otherShardName]: {
                bucketFilter: makeBucketFilter({
                    "$and": [
                        {"control.min.f": {"$_internalExprLte": 0}},
                        {"control.max.f": {"$_internalExprGte": 0}}
                    ]
                }),
                residualFilter: {"f": {"$eq": 0}},
                nBucketsUnpacked: 0,
                nMeasurementsMatched: 0,
                nMeasurementsUpdated: 0,
            }
        }
    },
    // This time field filter has the request targeted to the shard0.
    timeFilterOneShard: {
        updateList: [{
            q: {[timeField]: generateTimeValue(0), f: 0},
            u: [
                {$unset: "f"},
                {$set: {"newField": 1}},
            ],
            multi: true,
        }],
        expectedExplain: {
            [primaryShardName]: {
                bucketFilter: makeBucketFilter({
                    "$and": [
                        {
                            "$and": [
                                {
                                    "control.min.time":
                                        {"$_internalExprLte": ISODate("2000-01-01T00:00:00Z")}
                                },
                                {
                                    "control.min.time":
                                        {"$_internalExprGte": ISODate("1999-12-31T23:00:00Z")}
                                },
                                {
                                    "control.max.time":
                                        {"$_internalExprGte": ISODate("2000-01-01T00:00:00Z")}
                                },
                                {
                                    "control.max.time":
                                        {"$_internalExprLte": ISODate("2000-01-01T01:00:00Z")}
                                },
                            ]
                        },
                        {
                            "$and": [
                                {"control.min.f": {"$_internalExprLte": 0}},
                                {"control.max.f": {"$_internalExprGte": 0}}
                            ]
                        }
                    ]
                }),
                residualFilter:
                    {"$and": [{"f": {"$eq": 0}}, {[timeField]: {"$eq": generateTimeValue(0)}}]},
                nBucketsUnpacked: 1,
                nMeasurementsMatched: 1,
                nMeasurementsUpdated: 1,
            },
        },
    },
    // This time field filter has the request targeted to both shards.
    timeFilterTwoShards: {
        updateList: [{
            q: {
                $or: [
                    {$and: [{[timeField]: generateTimeValue(1)}, {"f": {"$eq": 1}}]},
                    {$and: [{[timeField]: generateTimeValue(3)}, {"f": {"$eq": 3}}]}
                ]
            },
            u: {$set: {f: ["arr", "ay"]}},
            multi: true,
        }],
        expectedExplain: {
            [primaryShardName]: {
                residualFilter: {
                    "$or": [
                        {"$and": [{"f": {"$eq": 1}}, {[timeField]: {"$eq": generateTimeValue(1)}}]},
                        {"$and": [{"f": {"$eq": 3}}, {[timeField]: {"$eq": generateTimeValue(3)}}]}
                    ]
                },
                nBucketsUnpacked: 1,
                nMeasurementsMatched: 1,
                nMeasurementsUpdated: 1,
            },
            [otherShardName]: {
                residualFilter: {
                    "$or": [
                        {"$and": [{"f": {"$eq": 1}}, {[timeField]: {"$eq": generateTimeValue(1)}}]},
                        {"$and": [{"f": {"$eq": 3}}, {[timeField]: {"$eq": generateTimeValue(3)}}]}
                    ]
                },
                nBucketsUnpacked: 1,
                nMeasurementsMatched: 1,
                nMeasurementsUpdated: 1,
            }
        }
    },
    // This meta field filter targets the primary shard.
    metaFilterOneShard: {
        updateList: [{
            q: {[metaField]: 1, f: 1},
            u: [
                {$unset: "f"},
                {$set: {"newField": 1}},
                {$set: {"_id": 200}},
            ],
            multi: true,
        }],
        expectedExplain: {
            [primaryShardName]: {
                bucketFilter: makeBucketFilter(
                    {"meta": {"$eq": 1}},
                    {
                        "$and": [
                            {"control.min.f": {"$_internalExprLte": 1}},
                            {"control.max.f": {"$_internalExprGte": 1}}
                        ]
                    },
                    ),
                residualFilter: {"f": {"$eq": 1}},
                nBucketsUnpacked: 1,
                nMeasurementsMatched: 1,
                nMeasurementsUpdated: 1,
            }
        }
    },
    // Meta + time filter has the request targeted to shard1.
    metaTimeFilterOneShard: {
        updateList: [{
            q: {[metaField]: 2, [timeField]: generateTimeValue(2), f: 2},
            u: {$set: {f: 1000}},
            multi: true,
        }],
        expectedExplain: {
            [otherShardName]: {
                residualFilter:
                    {"$and": [{"f": {"$eq": 2}}, {[timeField]: {"$eq": generateTimeValue(2)}}]},
                nBucketsUnpacked: 1,
                nMeasurementsMatched: 1,
                nMeasurementsUpdated: 1,
            }
        }
    },
    metaFilterTwoShards: {
        updateList: [
            {
                q: {$and: [{[metaField]: {$gt: 0}}, {$or: [{f: {$eq: 1}}, {f: {$eq: 3}}]}]},
                u: {$set: {"newField": 101}},
                multi: true,
            },
        ],
        expectedExplain: {
            [primaryShardName]: {
                bucketFilter: makeBucketFilter({"meta": {"$gt": 0}}, {
                    "$or": [
                        {
                            "$and": [
                                {"control.min.f": {"$_internalExprLte": 1}},
                                {"control.max.f": {"$_internalExprGte": 1}}
                            ]
                        },
                        {
                            "$and": [
                                {"control.min.f": {"$_internalExprLte": 3}},
                                {"control.max.f": {"$_internalExprGte": 3}}
                            ]
                        }
                    ]
                }),
                residualFilter: {"f": {"$in": [1, 3]}},
                nBucketsUnpacked: 1,
                nMeasurementsMatched: 1,
                nMeasurementsUpdated: 1,
            },
            [otherShardName]: {
                bucketFilter: makeBucketFilter({"meta": {"$gt": 0}}, {
                    "$or": [
                        {
                            "$and": [
                                {"control.min.f": {"$_internalExprLte": 1}},
                                {"control.max.f": {"$_internalExprGte": 1}}
                            ]
                        },
                        {
                            "$and": [
                                {"control.min.f": {"$_internalExprLte": 3}},
                                {"control.max.f": {"$_internalExprGte": 3}}
                            ]
                        }
                    ]
                }),
                residualFilter: {"f": {"$in": [1, 3]}},
                nBucketsUnpacked: 1,
                nMeasurementsMatched: 1,
                nMeasurementsUpdated: 1,
            },
        }
    },
    metaObjectFilterOneShard: {
        updateList: [{
            q: {[metaField]: {a: 2}, f: 2},
            u: {$set: {"newField": 101}},
            multi: true,
        }],
        expectedExplain: {
            [otherShardName]: {
                bucketFilter: makeBucketFilter(
                    {"meta": {"$eq": {"a": 2}}},
                    {
                        "$and": [
                            {"control.min.f": {"$_internalExprLte": 2}},
                            {"control.max.f": {"$_internalExprGte": 2}}
                        ]
                    },
                    ),
                residualFilter: {"f": {"$eq": 2}},
                nBucketsUnpacked: 1,
                nMeasurementsMatched: 1,
                nMeasurementsUpdated: 1,
            }
        }
    },
    // Meta object + time filter has the request targeted to shard1.
    metaObjectTimeFilterOneShard: {
        updateList: [{
            q: {[metaField]: {a: 2}, [timeField]: generateTimeValue(2), f: 2},
            u: {$set: {f: 2000}},
            multi: true,
        }],
        expectedExplain: {
            [otherShardName]: {
                residualFilter:
                    {"$and": [{"f": {"$eq": 2}}, {[timeField]: {"$eq": generateTimeValue(2)}}]},
                nBucketsUnpacked: 1,
                nMeasurementsMatched: 1,
                nMeasurementsUpdated: 1,
            }
        }
    },
    metaObjectFilterTwoShards: {
        updateList: [
            {
                q: {[metaField]: {a: 1}, f: 1},
                u: {$set: {"newField": 101}},
                multi: true,
            },
        ],
        expectedExplain: {
            [primaryShardName]: {
                bucketFilter: makeBucketFilter(
                    {"meta": {"$eq": {"a": 1}}},
                    {
                        "$and": [
                            {"control.min.f": {"$_internalExprLte": 1}},
                            {"control.max.f": {"$_internalExprGte": 1}}
                        ]
                    },
                    ),
                residualFilter: {"f": {"$eq": 1}},
                nBucketsUnpacked: 1,
                nMeasurementsMatched: 1,
                nMeasurementsUpdated: 1,
            }
        }
    },
    metaSubFieldFilterOneShard: {
        updateList: [{
            q: {[metaField + '.a']: 2, f: 2},
            u: [
                {$set: {"newField": 101}},
            ],
            multi: true,
        }],
        expectedExplain: {
            [otherShardName]: {
                bucketFilter: makeBucketFilter({"meta.a": {"$eq": 2}}, {
                    "$and": [
                        {"control.min.f": {"$_internalExprLte": 2}},
                        {"control.max.f": {"$_internalExprGte": 2}}
                    ]
                }),
                residualFilter: {"f": {"$eq": 2}},
                nBucketsUnpacked: 1,
                nMeasurementsMatched: 1,
                nMeasurementsUpdated: 1,
            }
        }
    },
    // Meta sub field + time filter has the request targeted to shard1.
    metaSubFieldTimeFilterOneShard: {
        updateList: [{
            q: {[metaField + '.a']: 2, [timeField]: generateTimeValue(2), f: 2},
            u: {$set: {"newField": 101}},
            multi: true,
        }],
        expectedExplain: {
            [otherShardName]: {
                residualFilter:
                    {"$and": [{"f": {"$eq": 2}}, {[timeField]: {"$eq": generateTimeValue(2)}}]},
                nBucketsUnpacked: 1,
                nMeasurementsMatched: 1,
                nMeasurementsUpdated: 1,
            }
        }
    },
    metaSubFieldFilterTwoShards: {
        updateList: [
            {
                q: {$and: [{[metaField + '.a']: {$gt: 0}}, {$or: [{f: {$eq: 1}}, {f: {$eq: 2}}]}]},
                u: {$set: {"newField": 101}},
                multi: true
            },
        ],
        expectedExplain: {
            [primaryShardName]: {
                bucketFilter: makeBucketFilter({"meta.a": {"$gt": 0}}, {
                    "$or": [
                        {
                            "$and": [
                                {"control.min.f": {"$_internalExprLte": 1}},
                                {"control.max.f": {"$_internalExprGte": 1}}
                            ]
                        },
                        {
                            "$and": [
                                {"control.min.f": {"$_internalExprLte": 2}},
                                {"control.max.f": {"$_internalExprGte": 2}}
                            ]
                        }
                    ]
                }),
                residualFilter: {"f": {"$in": [1, 2]}},
                nBucketsUnpacked: 1,
                nMeasurementsMatched: 1,
                nMeasurementsUpdated: 1,
            },
            [otherShardName]: {
                bucketFilter: makeBucketFilter({"meta.a": {"$gt": 0}}, {
                    "$or": [
                        {
                            "$and": [
                                {"control.min.f": {"$_internalExprLte": 1}},
                                {"control.max.f": {"$_internalExprGte": 1}}
                            ]
                        },
                        {
                            "$and": [
                                {"control.min.f": {"$_internalExprLte": 2}},
                                {"control.max.f": {"$_internalExprGte": 2}}
                            ]
                        }
                    ]
                }),
                residualFilter: {"f": {"$in": [1, 2]}},
                nBucketsUnpacked: 1,
                nMeasurementsMatched: 1,
                nMeasurementsUpdated: 1,
            }
        }
    }
};

function runExplainTest(collConfig, reqConfig, insertFn) {
    jsTestLog(`Running a test with configuration: ${tojson({collConfig, reqConfig})}`);

    // Prepares a sharded timeseries collection.
    const [coll, _] = TimeseriesMultiUpdateUtil.prepareShardedTimeseriesCollection(
        mongos, st, testDB, collName, collConfig, insertFn);

    // We can only run the explain on one update at a time.
    assert.eq(reqConfig.updateList.length,
              1,
              `The updateList can only contain one update: ${tojson(reqConfig)}`);
    const update = reqConfig.updateList[0];
    const expectedExplainOutput = reqConfig.expectedExplain;

    // Run explain on the update and examine the execution stages for the expected results.
    const explainOutput = assert.commandWorked(coll.runCommand(
        {explain: {update: coll.getName(), updates: [update]}, verbosity: "executionStats"}));
    const execStages = getExecutionStages(explainOutput);
    assert.eq(execStages.length,
              Object.keys(expectedExplainOutput).length,
              `Mismatch in expected explain: ${tojson(expectedExplainOutput)} and exec stages: ${
                  tojson(explainOutput)}`);

    for (const execStage of execStages) {
        // Based off of the shard name, extract corresponding expected output.
        const expectedExplainForShard = expectedExplainOutput[execStage.shardName];
        assert(expectedExplainForShard !== undefined,
               `No expected explain output included for the execution stage: ${tojson(execStage)}`);

        assert.eq("TS_MODIFY",
                  execStage.stage,
                  `TS_MODIFY stage not found in executionStages: ${tojson(execStage)}`);
        assert.eq("updateMany",
                  execStage.opType,
                  `TS_MODIFY stage not found in executionStages: ${tojson(execStage)}`);

        // Check the bucket and residual filters if they are provided in the expected explain
        // result.
        if (expectedExplainForShard.bucketFilter !== undefined) {
            assert.eq(expectedExplainForShard.bucketFilter,
                      execStage.bucketFilter,
                      `TS_MODIFY bucketFilter is wrong: ${tojson(execStage)}`);
        }
        if (expectedExplainForShard.residualFilter !== undefined) {
            assert.eq(expectedExplainForShard.residualFilter,
                      execStage.residualFilter,
                      `TS_MODIFY residualFilter is wrong: ${tojson(execStage)}`);
        }

        // Check the expected metrics for the expected explain result.
        assert.eq(expectedExplainForShard.nBucketsUnpacked,
                  execStage.nBucketsUnpacked,
                  `Got wrong nBucketsUnpacked: ${tojson(execStage)}`);
        assert.eq(expectedExplainForShard.nMeasurementsMatched,
                  execStage.nMeasurementsMatched,
                  `Got wrong nMeasurementsMatched: ${tojson(execStage)}`);
        assert.eq(expectedExplainForShard.nMeasurementsUpdated,
                  execStage.nMeasurementsUpdated,
                  `Got wrong nMeasurementsUpdated: ${tojson(execStage)}`);
    }
}

function runOneTestCase(collConfigName, reqConfigName) {
    const collConfig = TimeseriesMultiUpdateUtil.collectionConfigurations[collConfigName];
    const reqConfig = requestConfigurations[reqConfigName];

    TimeseriesTest.run((insertFn) => {
        jsTestLog("req config " + reqConfigName);
        runExplainTest(collConfig, reqConfig, insertFn);
    }, testDB);
}

runOneTestCase("metaShardKey", "emptyFilter");

runOneTestCase("metaShardKey", "nonShardKeyFilter");

runOneTestCase("timeShardKey", "timeFilterOneShard");

runOneTestCase("timeShardKey", "timeFilterTwoShards");

runOneTestCase("metaTimeShardKey", "metaTimeFilterOneShard");

runOneTestCase("metaObjectTimeShardKey", "metaObjectTimeFilterOneShard");

runOneTestCase("metaSubFieldTimeShardKey", "metaSubFieldTimeFilterOneShard");

runOneTestCase("metaShardKey", "metaFilterOneShard");

runOneTestCase("metaShardKey", "metaFilterTwoShards");

runOneTestCase("metaObjectShardKey", "metaObjectFilterOneShard");

runOneTestCase("metaObjectShardKey", "metaSubFieldFilterTwoShards");

runOneTestCase("metaObjectShardKey", "metaObjectFilterTwoShards");

runOneTestCase("metaSubFieldShardKey", "metaSubFieldFilterOneShard");

st.stop();
