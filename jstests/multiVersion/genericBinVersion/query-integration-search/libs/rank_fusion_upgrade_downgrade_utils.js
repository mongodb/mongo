import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";

export const rankFusionPipeline = [{$rankFusion: {input: {pipelines: {field: [{$sort: {foo: 1}}]}}}}];
export const rankFusionPipelineWithScoreDetails = [
    {$rankFusion: {input: {pipelines: {field: [{$sort: {foo: 1}}]}}, scoreDetails: true}},
    {$project: {scoreDetails: {$meta: "scoreDetails"}, score: {$meta: "score"}}},
];

const docs = [
    {_id: 0, foo: "xyz"},
    {_id: 1, foo: "bar"},
    {_id: 2, foo: "mongodb"},
];

export const collName = jsTestName();
export const getDB = (primaryConnection) => primaryConnection.getDB(jsTestName());

export function setupCollection(primaryConn, shardingTest = null) {
    const coll = assertDropAndRecreateCollection(getDB(primaryConn), collName);

    if (shardingTest) {
        shardingTest.shardColl(coll, {_id: 1});
    }

    assert.commandWorked(coll.insertMany(docs));
    assert.commandWorked(coll.createIndex({foo: "text"}));
}

/**
 * Override to allow running with an unsharded collection in a sharded cluster.
 */
export function setupUnshardedCollection(primaryConn, shardingTest = null) {
    setupCollection(primaryConn);
}

export function assertRankFusionAggregateAccepted(db, collName) {
    // $rankFusion succeeds in an aggregation command.
    assert.commandWorked(db.runCommand({aggregate: collName, pipeline: rankFusionPipeline, cursor: {}}));

    // $rankFusion with scoreDetails succeeds in an aggregation command.
    assert.commandWorked(
        db.runCommand({aggregate: collName, pipeline: rankFusionPipelineWithScoreDetails, cursor: {}}),
    );
}

/**
 * Verifies that existing stages whose behavior depends on the value of the rank fusion feature
 * flags work as expected.
 */
export function assertRefactoredMQLKeepsWorking(db) {
    {
        // Run a $lookup with a $setWindowFields. This covers the case
        // where an upgraded shard requests sort key metadata from a
        // non-upgraded shard.
        const results = db[collName]
            .aggregate([
                {
                    $lookup: {
                        from: collName,
                        pipeline: [{$setWindowFields: {sortBy: {numOccurrences: 1}, output: {rank: {$rank: {}}}}}],
                        as: "out",
                    },
                },
            ])
            .toArray();
        assert.gt(results.length, 0);
        assert.gt(results[0]["out"].length, 0, results);
    }

    {
        // Run a $text query that produces $textScore metadata. This covers
        // the case where shards generate implicit $score metadata before mongos
        // is upgraded.
        const results = db[collName]
            .aggregate([{$match: {$text: {$search: "xyz"}}}, {$sort: {score: {$meta: "textScore"}}}])
            .toArray();
        assert.eq(results, [{_id: 0, foo: "xyz"}]);
    }

    {
        // Run a $setWindowFields with and without optimizations enabled. This covers the case where mongos desugars the
        // pipeline and does not include outputSortKeyMetadata in the $sort, but mongod already has the feature enabled
        // and expects sort key metadata to exist. This specifically causes an issue when the sort is not pushed down.
        const setWindowFields = {
            $setWindowFields: {partitionBy: "$foo", sortBy: {bar: 1}, output: {rank: {$rank: {}}}},
        };
        let results = db[collName].aggregate(setWindowFields).toArray();
        assert.gt(results.length, 0);

        results = db[collName].aggregate([{$_internalInhibitOptimization: {}}, setWindowFields]).toArray();
        assert.gt(results.length, 0);
    }
}
