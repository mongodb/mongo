// Tests that commands like find, aggregate and update accepts a 'let' parameter which defines
// variables for use in expressions within the command.
// TODO SERVER-46707: move this back to core after let params work in sharded commands is complete.
// @tags: [assumes_against_mongod_not_mongos, requires_fcv46]

(function() {
"use strict";

const coll = db.command_let_variables;
const targetColl = db.command_let_variables_target;
coll.drop();

assert.commandWorked(coll.insert([
    {
        Species: "Blackbird (Turdus merula)",
        population_trends: [
            {term: {start: 1970, end: 2014}, pct_change: -16, annual: -0.38, trend: "no change"},
            {term: {start: 2009, end: 2014}, pct_change: -2, annual: -0.36, trend: "no change"}
        ]
    },
    {
        Species: "Bullfinch (Pyrrhula pyrrhula)",
        population_trends: [
            {term: {start: 1970, end: 2014}, pct_change: -39, annual: -1.13, trend: "no change"},
            {term: {start: 2009, end: 2014}, pct_change: 12, annual: 2.38, trend: "weak increase"}
        ]
    },
    {
        Species: "Chaffinch (Fringilla coelebs)",
        population_trends: [
            {term: {start: 1970, end: 2014}, pct_change: 27, annual: 0.55, trend: "no change"},
            {term: {start: 2009, end: 2014}, pct_change: -7, annual: -1.49, trend: "weak decline"}
        ]
    },
    {
        Species: "Song Thrush (Turdus philomelos)",
        population_trends: [
            {term: {start: 1970, end: 2014}, pct_change: -53, annual: -1.7, trend: "weak decline"},
            {term: {start: 2009, end: 2014}, pct_change: -4, annual: -0.88, trend: "no change"}
        ]
    }
]));

// Aggregate tests
const pipeline = [
    {$project: {_id: 0}},
    {$unwind: "$population_trends"},
    {$match: {$expr: {$eq: ["$population_trends.trend", "$$target_trend"]}}},
    {$sort: {Species: 1}}
];
let expectedResults = [{
    Species: "Bullfinch (Pyrrhula pyrrhula)",
    population_trends:
        {term: {start: 2009, end: 2014}, pct_change: 12, annual: 2.38, trend: "weak increase"}
}];
assert.eq(coll.aggregate(pipeline, {let : {target_trend: "weak increase"}}).toArray(),
          expectedResults);

expectedResults = [
    {
        Species: "Chaffinch (Fringilla coelebs)",
        population_trends:
            {term: {start: 2009, end: 2014}, pct_change: -7, annual: -1.49, trend: "weak decline"}
    },
    {
        Species: "Song Thrush (Turdus philomelos)",
        population_trends:
            {term: {start: 1970, end: 2014}, pct_change: -53, annual: -1.7, trend: "weak decline"}
    }
];
assert.eq(coll.aggregate(pipeline, {let : {target_trend: "weak decline"}}).toArray(),
          expectedResults);

// Test that if runtimeConstants and let are both specified, both will coexist.
let constants = {
    localNow: new Date(),
    clusterTime: new Timestamp(0, 0),
};

assert.eq(
    coll.aggregate(pipeline, {runtimeConstants: constants, let : {target_trend: "weak decline"}})
        .toArray(),
    expectedResults);

// Test that undefined let params in the pipeline fail gracefully.
assert.commandFailedWithCode(db.runCommand({
    aggregate: coll.getName(),
    pipeline: pipeline,
    runtimeConstants: constants,
    cursor: {},
    let : {cat: "not_a_bird"}
}),
                             17276);

// Test null and empty let parameters
const pipeline_no_lets = [
    {$project: {_id: 0}},
    {$unwind: "$population_trends"},
    {$match: {$expr: {$eq: ["$population_trends.trend", "weak decline"]}}},
    {$sort: {Species: 1}}
];
assert.eq(coll.aggregate(pipeline_no_lets, {runtimeConstants: constants, let : {}}).toArray(),
          expectedResults);

assert.commandFailedWithCode(db.runCommand({
    aggregate: coll.getName(),
    pipeline: pipeline_no_lets,
    runtimeConstants: constants,
    cursor: {},
    let : null
}),
                             ErrorCodes.TypeMismatch);

// Function to prepare target collection of $merge stage for testing.
function prepMergeTargetColl() {
    targetColl.drop();

    assert.commandWorked(db.runCommand({
        aggregate: coll.getName(),
        pipeline: [
            {$match: {$expr: {$eq: ["$Species", "Song Thrush (Turdus philomelos)"]}}},
            {$out: targetColl.getName()}
        ],
        cursor: {}
    }));
}

// Test that $merge stage can use 'let' variables within its own stage's pipeline.
prepMergeTargetColl();
assert.commandWorked(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [{
        $merge: {
            into: targetColl.getName(),
            let : {variable: "INNER"},
            whenMatched: [{$addFields: {"var": "$$variable"}}]
        }
    }],
    cursor: {}
}));
assert.eq(targetColl.aggregate({$match: {$expr: {$eq: ["$var", "INNER"]}}}).toArray().length, 1);

// Test that $merge stage can access command-level 'let' variables.
prepMergeTargetColl();
assert.commandWorked(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [
        {$merge: {into: targetColl.getName(), whenMatched: [{$addFields: {"var": "$$variable"}}]}}
    ],
    cursor: {},
    let : {variable: "OUTER"}
}));
assert.eq(targetColl.aggregate({$match: {$expr: {$eq: ["$var", "OUTER"]}}}).toArray().length, 1);

// Test that $merge stage can use stage-level and command-level 'let' variables in same command.
prepMergeTargetColl();
assert.commandWorked(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [{
        $merge: {
            into: targetColl.getName(),
            let : {stage: "INNER"},
            whenMatched: [{$addFields: {"innerVar": "$$stage", "outerVar": "$$command"}}]
        }
    }],
    cursor: {},
    let : {command: "OUTER"}
}));
assert.eq(
    targetColl
        .aggregate({
            $match: {
                $and:
                    [{$expr: {$eq: ["$innerVar", "INNER"]}},
                     {$expr: {$eq: ["$outerVar", "OUTER"]}}]
            }
        })
        .toArray()
        .length,
    1);

// Test that $merge stage follows variable scoping rules with stage-level and command-level 'let'
// variables.
prepMergeTargetColl();
assert.commandWorked(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [{
        $merge: {
            into: targetColl.getName(),
            let : {variable: "INNER"},
            whenMatched: [{$addFields: {"var": "$$variable"}}]
        }
    }],
    cursor: {},
    let : {variable: "OUTER"}
}));
assert.eq(targetColl.aggregate({$match: {$expr: {$eq: ["$var", "INNER"]}}}).toArray().length, 1);
}());
