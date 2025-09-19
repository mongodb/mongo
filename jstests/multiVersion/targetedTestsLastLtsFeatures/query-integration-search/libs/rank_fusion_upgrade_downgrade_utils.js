import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";

export const rankFusionPipeline =
    [{$rankFusion: {input: {pipelines: {field: [{$sort: {foo: 1}}]}}}}];
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

export function assertRankFusionAggregateAccepted(db, collName) {
    // $rankFusion succeeds in an aggregation command.
    assert.commandWorked(
        db.runCommand({aggregate: collName, pipeline: rankFusionPipeline, cursor: {}}));

    // $rankFusion with scoreDetails succeeds in an aggregation command.
    assert.commandWorked(
        db.runCommand(
            {aggregate: collName, pipeline: rankFusionPipelineWithScoreDetails, cursor: {}}),
    );
}
