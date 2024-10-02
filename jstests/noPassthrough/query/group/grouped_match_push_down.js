// Tests classic planner rule to push-down $match with filters on renamed fields with dotted paths
// over $group and $project in aggregation pipelines.

import {getEngine, getWinningPlanFromExplain} from "jstests/libs/query/analyze_plan.js";

const conn = MongoRunner.runMongod();
const db = conn.getDB('test');

const MultiStageSBE = 0;
const MultiStageClassic = 1;
const SingleStage = 2;

// Given the root stage of agg explain's JSON representation of a query plan ('root'), returns
// the pipeline stage names included in the plan in order of execution. The resulting array includes
// only first-level stages (does not include stages within $group, $lookup, $union etc).
function getAggStageSequence(explain) {
    let stageSequence = [];
    if (explain.hasOwnProperty("stages")) {
        for (let j = 0; j < explain.stages.length; j++) {
            let stageName = Object.keys(explain.stages[j]);
            stageSequence.push(stageName[0]);
        }
    }
    return stageSequence;
}

// Given the winningplan from the explain output of a query, this method returns the array of the
// stage names lowered into SBE.
function getSingleStageSequence(winningPlan) {
    let stageSequence = [];
    while (winningPlan.hasOwnProperty("stage")) {
        stageSequence.push(winningPlan.stage);
        if (!winningPlan.hasOwnProperty("inputStage")) {
            break;
        }
        winningPlan = winningPlan.inputStage;
    }
    return stageSequence;
}

// Gets the sequence of stages from an explain output.
// Takes as input the full explain outputs (from the root) as well as the nested explain output from
// a shard. The function returns a tuple of the extracted stage sequence from explain outputs as
// well as the recognized system configuration. The system configuration is used to identify the
// expected result to compare against.
function getStageSequenceToCompare(explainOutput, queryEngine) {
    if (explainOutput.hasOwnProperty("stages")) {
        let stageSequence = getAggStageSequence(explainOutput);

        // Varying expected plan depeding on the query engine.
        return [stageSequence, queryEngine == "classic" ? MultiStageClassic : MultiStageSBE];
    } else {
        let winningPlan = getWinningPlanFromExplain(explainOutput);
        let stageSequenceSBE = getSingleStageSequence(winningPlan);

        return [stageSequenceSBE, SingleStage];
    }
}

// Asserts end-to-end the optimization of group-project-match stage sequence.
// It evaluates the correctness of the result, as well as whether the
// optimization rules have been applied correctly.
function assertPipelineOptimizationAndResult({pipeline, expectedStageSequence, expectedResult}) {
    assertPipelineOptimization(pipeline, expectedStageSequence);
    assertQueryResult(pipeline, expectedResult);
}

// Asserts whether the provided aggregation pipeline has the expected sequence of stages.
// The function parses explain outputs, and treats each explain output variation accordingly.
function assertPipelineOptimization(pipeline, expectedStageSequence) {
    const explain = coll.explain().aggregate(pipeline);
    let queryEngine = getEngine(explain);

    if (explain.hasOwnProperty("shards")) {
        for (const shardName in explain.shards) {
            let stageSequenceTuple =
                getStageSequenceToCompare(explain.shards[shardName], queryEngine);
            assert.eq(stageSequenceTuple[0], expectedStageSequence[stageSequenceTuple[1]], explain);
        }
    } else {
        let stageSequenceTuple = getStageSequenceToCompare(explain, queryEngine);
        assert.eq(stageSequenceTuple[0], expectedStageSequence[stageSequenceTuple[1]], explain);
    }
}

// Asserts whether the provided aggregation pipeline outputs the correct set of results.
// The function requires access to the collection to run the query.
function assertQueryResult(pipeline, expectedResult) {
    const actualResult = coll.aggregate(pipeline).toArray();
    assert.sameMembers(expectedResult, actualResult, coll.explain().aggregate(pipeline));
}

const coll = db.grouped_match_push_down;
coll.drop();

assert.commandWorked(coll.insert({_id: 1, x: 10}));
assert.commandWorked(coll.insert({_id: 2, x: 20}));
assert.commandWorked(coll.insert({_id: 3, x: 30}));
assert.commandWorked(coll.insert({_id: 20, d: 2}));

// Asserts that a sequence of stages group, project, match over a rename on a dotted path (depth
// 3) will push the predicate before the group stage.
assertPipelineOptimizationAndResult({
    pipeline: [
        {$group: {_id: {c: '$d'}, c: {$sum: {$const: 1}}}},
        {$project: {m: '$_id.c'}},
        {$match: {m: {$eq: 2}}}
    ],
    expectedStageSequence: {
        [MultiStageSBE]: ["$cursor", "$project"],
        [MultiStageClassic]: ["$cursor", "$group", "$project"],
        [SingleStage]: ["PROJECTION_DEFAULT", "GROUP", "COLLSCAN"]
    },
    expectedResult: [{"_id": {"c": 2}, "m": 2}]
});

// Asserts that the optimization over group, project, match over a renamed dotted path will push
// down the predicate while the dotted notation is kept to 2 levels.
assertPipelineOptimizationAndResult({
    pipeline: [
        {$group: {_id: {c: '$d'}, c: {$sum: {$const: 1}}}},
        {$project: {m: '$_id.c'}},
        {$project: {m2: '$m'}},
        {$match: {m2: {$eq: 2}}}
    ],
    expectedStageSequence: {
        [MultiStageSBE]: ["$cursor", "$project", "$project"],
        [MultiStageClassic]: ["$cursor", "$group", "$project", "$project"],
        [SingleStage]: ["PROJECTION_DEFAULT", "PROJECTION_DEFAULT", "GROUP", "COLLSCAN"]
    },
    expectedResult: [{"_id": {"c": 2}, "m2": 2}]
});

// Asserts that the optimization over group, project, match over a renamed dotted path will not
// push down the predicate when the rename stage renames a dotted path with depth more than 3.
assertPipelineOptimizationAndResult({
    pipeline: [
        {$group: {_id: {c: {d: '$d'}}, c: {$sum: {$const: 1}}}},
        {$project: {m: '$_id.c.d'}},
        {$match: {m: {$eq: 2}}}
    ],
    expectedStageSequence: {
        [MultiStageSBE]: ["$cursor", "$project", "$match"],
        [MultiStageClassic]: ["$cursor", "$group", "$project", "$match"],
        [SingleStage]: ["MATCH", "PROJECTION_DEFAULT", "GROUP", "COLLSCAN"]
    },
    expectedResult: [{"_id": {"c": {"d": 2}}, "m": 2}]
});

// Asserts that the optimization over group, addFields, match over a renamed dotted path will not
// lose other dependencies between stages.
assertPipelineOptimizationAndResult({
    pipeline: [
        {$group: {_id: "$d", c: {$sum: {$const: 1}}}},
        {$addFields: {c: '$_id'}},
        {$match: {c: {$eq: 2}}}
    ],
    expectedStageSequence: {
        [MultiStageSBE]: ["$cursor", "$addFields"],
        [MultiStageClassic]: ["$cursor", "$group", "$addFields"],
        [SingleStage]: ["PROJECTION_DEFAULT", "GROUP", "COLLSCAN"]
    },
    expectedResult: [{_id: 2, c: 2}]
});

assertPipelineOptimizationAndResult({
    pipeline: [
        {$group: {_id: "$d", c: {$sum: {$const: 1}}}},
        {$addFields: {d: '$c'}},
        {$match: {d: {$eq: 1}}}
    ],
    expectedStageSequence: {
        [MultiStageSBE]: ["$cursor", "$match", "$addFields"],
        [MultiStageClassic]: ["$cursor", "$group", "$match", "$addFields"],
        [SingleStage]: ["PROJECTION_DEFAULT", "MATCH", "GROUP", "COLLSCAN"]
    },
    expectedResult: [{_id: 2, c: 1, d: 1}]
});

MongoRunner.stopMongod(conn);
