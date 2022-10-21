/**
 * Tests that the aggregate command can use command-level let variables with $merge.
 *
 * @tags: [
 *   assumes_unsharded_collection,
 *   does_not_support_stepdowns,
 *   does_not_support_causal_consistency,
 * ]
 */
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

// Test that expressions wrapped with $literal are serialized correctly in combination with
// pipelines containing $merge.
prepMergeTargetColl();
assert.commandWorked(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [{$project: {var : "$$variable"}}, {$merge: {into: targetColl.getName()}}],
    let : {variable: {$literal: "$notAFieldPath"}},
    cursor: {}
}));
assert.eq(targetColl.aggregate({$match: {$expr: {$eq: ["$var", {$literal: "$notAFieldPath"}]}}})
              .toArray()
              .length,
          4);

prepMergeTargetColl();
assert.commandWorked(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [{
        $merge: {
            into: targetColl.getName(),
            let : {variable: {$literal: "$notAFieldPath"}},
            whenMatched: [{$addFields: {"var": "$$variable"}}]
        }
    }],
    cursor: {},
}));
assert.eq(targetColl.aggregate({$match: {$expr: {$eq: ["$var", {$literal: "$notAFieldPath"}]}}})
              .toArray()
              .length,
          1);
}());
