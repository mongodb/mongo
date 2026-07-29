/*
 * Tests hybrid search with both $scoreFusion and $rankFusion get rejected when inside of $unionWith
 * or $lookup subpipelines on timeseries collections.
 *
 * @tags: [ requires_timeseries, featureFlagSearchHybridScoringFull, requires_fcv_82, requires_getmore ]
 */

const timeseriesCollName = jsTestName() + "_timeseries";
assert.commandWorked(
    db.createCollection(timeseriesCollName, {timeseries: {timeField: "t", metaField: "m"}}),
);
const timeseriesColl = db[timeseriesCollName];
assert.commandWorked(timeseriesColl.insert({t: new Date(), m: 1, a: 42, b: 17}));

const nonTimeseriesCollName = jsTestName() + "_nontimeseries";
assert.commandWorked(db.createCollection(nonTimeseriesCollName));
const nonTimeseriesColl = db[nonTimeseriesCollName];
assert.commandWorked(nonTimeseriesColl.insert({a: 50, b: 20}));

let rankFusionPipeline = [{$rankFusion: {input: {pipelines: {sortPipeline: [{$sort: {a: 1}}]}}}}];
let scoreFusionPipeline = [
    {
        $scoreFusion: {
            input: {pipelines: {scorePipeline: [{$score: {score: "$a"}}]}, normalization: "none"},
        },
    },
];

function runPipeline(pipeline, collName) {
    // Run the aggregation and fully drain the resulting cursor. The firstBatch in $unionWith might
    // not include timeseries documents, and therefore might succeed, since the subpipeline hasn't
    // been validated yet.
    const initialResponse = db.runCommand({aggregate: collName, pipeline, cursor: {}});
    if (!initialResponse.ok) {
        return initialResponse;
    }
    let cursor = initialResponse.cursor;
    while (cursor.id != 0) {
        const getMoreResponse = db.runCommand({getMore: cursor.id, collection: collName});
        if (!getMoreResponse.ok) {
            return getMoreResponse;
        }
        cursor = getMoreResponse.cursor;
    }
    return initialResponse;
}

(function testHybridSearchRejected() {
    assert.commandFailedWithCode(runPipeline(rankFusionPipeline, timeseriesCollName), [
        10557301, // shard
        10557300, // router
        ErrorCodes.OptionNotSupportedOnView,
    ]);
    assert.commandFailedWithCode(runPipeline(scoreFusionPipeline, timeseriesCollName), [
        10557301, // shard
        10557300, // router
        ErrorCodes.OptionNotSupportedOnView,
    ]);
})();

(function testUnionWithRejectsIsHybridSearchFlagFromUser() {
    let badUnionWithStageWithIsHybridSearchTrue = {
        $unionWith: {
            coll: timeseriesCollName,
            pipeline: [{$sort: {_id: 1}}],
            $_internalIsHybridSearch: true,
        },
    };
    assert.commandFailedWithCode(
        runPipeline([badUnionWithStageWithIsHybridSearchTrue], timeseriesCollName),
        5491300,
    );

    let badUnionWithStageWithIsHybridSearchFalse = {
        $unionWith: {
            coll: timeseriesCollName,
            pipeline: [{$sort: {_id: 1}}],
            $_internalIsHybridSearch: false,
        },
    };
    assert.commandFailedWithCode(
        runPipeline([badUnionWithStageWithIsHybridSearchFalse], timeseriesCollName),
        5491300,
    );
})();

(function testLookupRejectsIsHybridSearchFlagFromUser() {
    let badLookupStageWithIsHybridSearchTrue = {
        $lookup: {
            from: timeseriesCollName,
            pipeline: [{$sort: {_id: 1}}],
            as: "out",
            $_internalIsHybridSearch: true,
        },
    };

    assert.commandFailedWithCode(
        runPipeline([badLookupStageWithIsHybridSearchTrue], timeseriesCollName),
        5491300,
    );

    let badLookupStageWithIsHybridSearchFalse = {
        $lookup: {
            from: timeseriesCollName,
            pipeline: [{$sort: {_id: 1}}],
            as: "out",
            $_internalIsHybridSearch: false,
        },
    };
    assert.commandFailedWithCode(
        runPipeline([badLookupStageWithIsHybridSearchFalse], timeseriesCollName),
        5491300,
    );
})();

// Note that hybrid search cannot run against a collectionless $unionWith because a collectionless
// $unionWith must start with the $documents stage, but hybrid search stages must be the first
// stages in the pipeline.

(function testHybridSearchRejectedOnUnionWithPipeline() {
    let rankFusionUnionWithStage = {
        $unionWith: {coll: timeseriesCollName, pipeline: rankFusionPipeline},
    };
    assert.commandFailedWithCode(
        runPipeline([rankFusionUnionWithStage], timeseriesCollName),
        [10787900, 10787901],
    );

    let scoreFusionUnionWithStage = {
        $unionWith: {coll: timeseriesCollName, pipeline: scoreFusionPipeline},
    };
    assert.commandFailedWithCode(
        runPipeline([scoreFusionUnionWithStage], timeseriesCollName),
        [10787900, 10787901],
    );
})();

(function testHybridSearchOnUnionWithOnNonTimeseriesCollectionInsideTimeseriesQuery() {
    // These queries should pass because hybrid search is valid on a non-timeseries collection,
    // regardless of what the outer query is running on.
    let rankFusionUnionWithStage = {
        $unionWith: {coll: nonTimeseriesCollName, pipeline: rankFusionPipeline},
    };
    assert.commandWorked(runPipeline([rankFusionUnionWithStage], timeseriesCollName));

    let scoreFusionUnionWithStage = {
        $unionWith: {coll: nonTimeseriesCollName, pipeline: scoreFusionPipeline},
    };
    assert.commandWorked(runPipeline([scoreFusionUnionWithStage], timeseriesCollName));
})();

(function testHybridSearchOnUnionWithOnTimeseriesCollectionInsideNonTimeseriesQuery() {
    // These queries should fail because hybrid search is not valid on timeseries collections,
    // regardless of what the outer query is running on.
    let rankFusionUnionWithStage = {
        $unionWith: {coll: timeseriesCollName, pipeline: rankFusionPipeline},
    };
    assert.commandFailedWithCode(
        runPipeline([rankFusionUnionWithStage], nonTimeseriesCollName),
        [10787900, 10787901, 12093200],
    );

    let scoreFusionUnionWithStage = {
        $unionWith: {coll: timeseriesCollName, pipeline: scoreFusionPipeline},
    };
    assert.commandFailedWithCode(
        runPipeline([scoreFusionUnionWithStage], nonTimeseriesCollName),
        [10787900, 10787901, 12093200],
    );
})();

(function testHybridSearchOnUnionWithOnTimeseriesCollectionInsideNonTimeseriesQueryNested() {
    let rankFusionUnionWithStage = {
        $unionWith: {coll: timeseriesCollName, pipeline: rankFusionPipeline},
    };
    let nestedRankFusionUnionWithStage = {
        $unionWith: {coll: nonTimeseriesCollName, pipeline: [rankFusionUnionWithStage]},
    };
    assert.commandFailedWithCode(
        runPipeline([nestedRankFusionUnionWithStage], nonTimeseriesCollName),
        [10787900, 10787901, 12093200],
    );

    let scoreFusionUnionWithStage = {
        $unionWith: {coll: timeseriesCollName, pipeline: scoreFusionPipeline},
    };
    let nestedScoreFusionUnionWithStage = {
        $unionWith: {coll: nonTimeseriesCollName, pipeline: [scoreFusionUnionWithStage]},
    };
    assert.commandFailedWithCode(
        runPipeline([nestedScoreFusionUnionWithStage], nonTimeseriesCollName),
        [10787900, 10787901, 12093200],
    );
})();

(function testHybridSearchRejectedOnLookupPipeline() {
    let rankFusionLookupStage = {
        $lookup: {from: timeseriesCollName, pipeline: rankFusionPipeline, as: "out"},
    };
    assert.commandFailedWithCode(
        runPipeline([rankFusionLookupStage], timeseriesCollName),
        [10787900, 10787901],
    );

    let scoreFusionLookupStage = {
        $lookup: {from: timeseriesCollName, pipeline: scoreFusionPipeline, as: "out"},
    };
    assert.commandFailedWithCode(
        runPipeline([scoreFusionLookupStage], timeseriesCollName),
        [10787900, 10787901],
    );
})();

(function testHybridSearchOnLookupOnNonTimeseriesCollectionInsideTimeseriesQuery() {
    // These queries should succeed because the pipeline is running against a non timeseries
    // collection.
    let rankFusionLookupStage = {
        $lookup: {from: nonTimeseriesCollName, pipeline: rankFusionPipeline, as: "out"},
    };
    assert.commandWorked(runPipeline([rankFusionLookupStage], timeseriesCollName));

    let scoreFusionLookupStage = {
        $lookup: {from: nonTimeseriesCollName, pipeline: scoreFusionPipeline, as: "out"},
    };
    assert.commandWorked(runPipeline([scoreFusionLookupStage], timeseriesCollName));
})();

(function testHybridSearchOnLookupOnTimeseriesCollectionInsideNonTimeseriesQuery() {
    let rankFusionLookupStage = {
        $lookup: {from: timeseriesCollName, pipeline: rankFusionPipeline, as: "out"},
    };
    assert.commandFailedWithCode(
        runPipeline([rankFusionLookupStage], nonTimeseriesCollName),
        [10787900, 10787901],
    );

    let scoreFusionLookupStage = {
        $lookup: {from: timeseriesCollName, pipeline: scoreFusionPipeline, as: "out"},
    };
    assert.commandFailedWithCode(
        runPipeline([scoreFusionLookupStage], nonTimeseriesCollName),
        [10787900, 10787901],
    );
})();

(function testHybridSearchOnLookupOnTimeseriesCollectionInsideNonTimeseriesQueryNested() {
    let rankFusionLookupStage = {
        $lookup: {from: timeseriesCollName, pipeline: rankFusionPipeline, as: "out"},
    };
    let nestedLookupRankFusionStage = {
        $lookup: {from: nonTimeseriesCollName, pipeline: [rankFusionLookupStage], as: "out"},
    };
    assert.commandFailedWithCode(
        runPipeline([nestedLookupRankFusionStage], nonTimeseriesCollName),
        [10787900, 10787901],
    );

    let scoreFusionLookupStage = {
        $lookup: {from: timeseriesCollName, pipeline: scoreFusionPipeline, as: "out"},
    };
    let nestedLookupScoreFusionStage = {
        $lookup: {from: nonTimeseriesCollName, pipeline: [scoreFusionLookupStage], as: "out"},
    };
    assert.commandFailedWithCode(
        runPipeline([nestedLookupScoreFusionStage], nonTimeseriesCollName),
        [10787900, 10787901],
    );
})();
