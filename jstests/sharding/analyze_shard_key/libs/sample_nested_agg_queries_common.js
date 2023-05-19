/**
 * Utilities for testing basic support for sampling nested aggregate queries (i.e. ones inside
 * $lookup, $graphLookup, $unionWith) on a sharded cluster.
 */

load("jstests/libs/sbe_util.js");  // For checkSBEEnabled.

// Make the periodic jobs for refreshing sample rates and writing sampled queries and diffs have a
// period of 1 second to speed up the test.
const queryAnalysisSamplerConfigurationRefreshSecs = 1;
const queryAnalysisWriterIntervalSecs = 1;

const outerAggTestCases = [
    // The test cases for singly-nested aggregate queries.
    {
        name: "lookup_custom_pipeline",
        supportCustomPipeline: true,
        makeOuterPipelineFunc: (localCollName, foreignCollName, pipeline) => {
            return [{$lookup: {from: foreignCollName, as: "joined", pipeline}}];
        },
        requireShardToRouteFunc: (db, collName, isShardedColl) => true
    },
    {
        name: "lookup_non_custom_pipeline",
        supportCustomPipeline: false,
        makeOuterPipelineFunc: (localCollName, foreignCollName, pipeline) => {
            return [
                {$lookup: {from: foreignCollName, as: "joined", localField: "a", foreignField: "x"}}
            ];
        },
        requireShardToRouteFunc: (db, collName, isShardedColl) => {
            const listCollectionRes =
                assert.commandWorked(db.runCommand({listCollections: 1, filter: {name: collName}}));
            const isClusteredColl =
                listCollectionRes.cursor.firstBatch[0].options.hasOwnProperty("clusteredIndex");

            // When SBE is enabled, if the collection is not sharded and either not clustered or
            // the featureFlagSbeFull is true, the shard will not create a separate pipeline to
            // execute the inner side of a $lookup stage so there is no nested aggregate query to
            // route. These are the cases when SBE is actually used; SBE does $lookup pushdown
            // whereas Classic does not.
            // TODO SERVER-75715: Remove "featureFlagSbeFull" comment reference and check, as this
            // ticket will move SBE clustered collection support out from behind this flag. The full
            // check should then become just "!isShardedColl && checkSBEEnabled(db)".
            const isEligibleForSBELookupPushdown = !isShardedColl &&
                ((!isClusteredColl && checkSBEEnabled(db)) ||
                 checkSBEEnabled(db, ["featureFlagSbeFull"]));
            return !isEligibleForSBELookupPushdown;
        }
    },
    {
        name: "unionWith",
        supportCustomPipeline: true,
        makeOuterPipelineFunc: (localCollName, foreignCollName, pipeline) => {
            return [{$unionWith: {coll: foreignCollName, pipeline}}];
        },
        requireShardToRouteFunc: (db, collName, isShardedColl) => true
    },
    {
        name: "graphLookup",
        supportCustomPipeline: false,
        makeOuterPipelineFunc: (localCollName, foreignCollName) => {
            return [{
                    $graphLookup: {
                        from: foreignCollName,
                        startWith: "$x",
                        connectFromField: "x",
                        connectToField: "a",
                        maxDepth: 1,
                        as: "connected"
                    }
                }];
        },
        requireShardToRouteFunc: (db, collName, isShardedColl) => true
    },
    // The test cases for doubly-nested aggregate queries.
    {
        name: "lookup+lookup",
        supportCustomPipeline: true,
        makeOuterPipelineFunc: (localCollName, foreignCollName, pipeline) => {
            return [{
                    $lookup: {
                        from: localCollName,
                        as: "joined",
                        pipeline: [{
                            $lookup: {
                                from: foreignCollName,
                                as: "joined",
                                pipeline
                            }
                        }]
                    }
                }];
        },
        requireShardToRouteFunc: (db, collName, isShardedColl) => true
    },
    {
        name: "lookup+unionWith",
        supportCustomPipeline: true,
        makeOuterPipelineFunc: (localCollName, foreignCollName, pipeline) => {
            return [{
                    $lookup: {
                        from: localCollName,
                        as: "joined",
                        pipeline: [{
                            $unionWith: {
                                coll: foreignCollName,
                                pipeline
                            }
                        }]
                    }
                }];
        },
        requireShardToRouteFunc: (db, collName, isShardedColl) => true
    },
    {
        name: "lookup+graphLookUp",
        supportCustomPipeline: false,
        makeOuterPipelineFunc: (localCollName, foreignCollName, pipeline) => {
            return [{
                    $lookup: {
                        from: localCollName,
                        as: "joined",
                        pipeline: [{
                            $graphLookup: {
                                from: foreignCollName,
                                startWith: "$x",
                                connectFromField: "x",
                                connectToField: "a",
                                maxDepth: 1,
                                as: "connected"
                            }
                        }]
                    }
                }];
        },
        requireShardToRouteFunc: (db, collName, isShardedColl) => true
    },
    {
        name: "unionWith+lookup",
        supportCustomPipeline: true,
        makeOuterPipelineFunc: (localCollName, foreignCollName, pipeline) => {
            return [{
                $unionWith: {
                    coll: localCollName,
                    pipeline: [{$lookup: {from: foreignCollName, as: "joined", pipeline}}]
                }
            }];
        },
        requireShardToRouteFunc: (db, collName, isShardedColl) => true
    },
    {
        name: "unionWith+unionWith",
        supportCustomPipeline: true,
        makeOuterPipelineFunc: (localCollName, foreignCollName, pipeline) => {
            return [{
                $unionWith: {
                    coll: localCollName,
                    pipeline: [{$unionWith: {coll: foreignCollName, pipeline}}]
                }
            }];
        },
        requireShardToRouteFunc: (db, collName, isShardedColl) => true
    },
    {
        name: "unionWith+graphLookup",
        supportCustomPipeline: false,
        makeOuterPipelineFunc: (localCollName, foreignCollName, pipeline) => {
            return [{
                    $unionWith: {
                        coll: localCollName,
                        pipeline: [{
                            $graphLookup: {
                                from: foreignCollName,
                                startWith: "$x",
                                connectFromField: "x",
                                connectToField: "a",
                                maxDepth: 1,
                                as: "connected"
                            }
                        }]
                    }
                }];
        },
        requireShardToRouteFunc: (db, collName, isShardedColl) => true,
    },
    {
        name: "facet",
        supportCustomPipeline: false,
        makeOuterPipelineFunc: (localCollName, foreignCollName, pipeline) => {
            return [{$facet: {foo: [{$match: {}}]}}];
        },
        requireShardToRouteFunc: (db, collName, isShardedColl) => false,
    }
];

const innerAggTestCases = [
    {
        // The filter is in the first stage.
        containInitialFilter: true,
        makeInnerPipelineFunc: (filter) => {
            return [{$match: filter}];
        }
    },
    {
        // The filter is not in the first stage but the stage that it is in is moveable.
        containInitialFilter: true,
        makeInnerPipelineFunc: (filter) => {
            return [{$sort: {x: -1}}, {$match: filter}];
        }
    },
    {
        // The filter is not in the first stage and the stage that it is in is not moveable.
        containInitialFilter: false,
        makeInnerPipelineFunc: (filter) => {
            return [{$_internalInhibitOptimization: {}}, {$match: filter}];
        }
    }
];

/**
 * Tests that a nested aggregate query run internally by an aggregation stage that takes in a
 * "pipeline" is sampled correctly.
 */
function testCustomInnerPipeline(makeOuterPipelineFunc,
                                 makeInnerPipelineFunc,
                                 containInitialFilter,
                                 st,
                                 dbName,
                                 localCollName,
                                 foreignCollName,
                                 filter,
                                 shardNames,
                                 explain,
                                 requireShardToRoute) {
    const mongosDB = st.s.getDB(dbName);
    const foreignNs = dbName + "." + foreignCollName;
    const foreignCollUuid = QuerySamplingUtil.getCollectionUuid(mongosDB, foreignCollName);

    let expectedSampledQueryDocs = [];

    const collation = QuerySamplingUtil.generateRandomCollation();
    const innerPipeline = makeInnerPipelineFunc(filter);
    const outerPipeline = makeOuterPipelineFunc(localCollName, foreignCollName, innerPipeline);
    const originalCmdObj =
        {aggregate: localCollName, pipeline: outerPipeline, collation, cursor: {}};

    jsTest.log("Testing command " + tojsononeline({originalCmdObj, explain}));
    assert.commandWorked(mongosDB.runCommand(explain ? {explain: originalCmdObj} : originalCmdObj));

    // Queries that are part of an 'explain' or that do not require shards to route should not get
    // sampled.
    if (!explain && requireShardToRoute) {
        const expectedFilter = containInitialFilter ? filter : {};
        const expectedDoc = {
            cmdName: "aggregate",
            cmdObj: {filter: expectedFilter, collation},
            shardNames
        };
        if (filter) {
            expectedDoc.filter = {"cmd.filter": expectedFilter};
        }
        expectedSampledQueryDocs.push(expectedDoc);
    }

    sleep(queryAnalysisWriterIntervalSecs * 1000);
    QuerySamplingUtil.assertSoonSampledQueryDocumentsAcrossShards(
        st, foreignNs, foreignCollUuid, ["aggregate"], expectedSampledQueryDocs);
    QuerySamplingUtil.clearSampledQueryCollectionOnAllShards(st);
}

/**
 * Tests that a nested aggregate query run internally by an aggregation stage that does not take in
 * a "pipeline" is sampled correctly.
 */
function testNoCustomInnerPipeline(makeOuterPipelineFunc,
                                   st,
                                   dbName,
                                   localCollName,
                                   foreignCollName,
                                   explain,
                                   requireShardToRoute) {
    const mongosDB = st.s.getDB(dbName);
    const foreignNs = dbName + "." + foreignCollName;
    const foreignCollUuid = QuerySamplingUtil.getCollectionUuid(mongosDB, foreignCollName);

    let expectedSampledQueryDocs = [];

    const collation = QuerySamplingUtil.generateRandomCollation();
    const outerPipeline = makeOuterPipelineFunc(localCollName, foreignCollName);
    const originalCmdObj =
        {aggregate: localCollName, pipeline: outerPipeline, collation, cursor: {}};

    jsTest.log("Testing command " + tojsononeline({originalCmdObj, explain}));
    assert.commandWorked(mongosDB.runCommand(explain ? {explain: originalCmdObj} : originalCmdObj));

    // Queries that are part of an 'explain' or that do not require shards to route should not get
    // sampled.
    if (!explain && requireShardToRoute) {
        // Out of the aggregation stages above, the only stages that doesn't take in a custom
        // pipeline are $graphLookup and $lookup (without a "pipeline" field). To avoid relying on
        // the current format of the $match filter that they internally construct, skip verifying
        // the filter and only verify that the query is present in the config.sampledQueries
        // collection.
        expectedSampledQueryDocs.push({});
    }

    sleep(queryAnalysisWriterIntervalSecs * 1000);
    QuerySamplingUtil.assertSoonSampledQueryDocumentsAcrossShards(
        st, foreignNs, foreignCollUuid, ["aggregate"], expectedSampledQueryDocs);
    QuerySamplingUtil.clearSampledQueryCollectionOnAllShards(st);
}
