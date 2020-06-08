// Tests that commands like find, aggregate and update accepts a 'let' parameter which defines
// variables for use in expressions within the command.
// @tags: [requires_fcv_46]
//
(function() {
"use strict";

load("jstests/libs/fixture_helpers.js");  // For 'isMongos' and 'isSharded'.

const coll = db.command_let_variables;
const targetColl = db.command_let_variables_target;

coll.drop();

const testDocs = [
    {
        _id: 1,
        Species: "Blackbird (Turdus merula)",
        population_trends: [
            {term: {start: 1970, end: 2014}, pct_change: -16, annual: -0.38, trend: "no change"},
            {term: {start: 2009, end: 2014}, pct_change: -2, annual: -0.36, trend: "no change"}
        ]
    },
    {
        _id: 2,
        Species: "Bullfinch (Pyrrhula pyrrhula)",
        population_trends: [
            {term: {start: 1970, end: 2014}, pct_change: -39, annual: -1.13, trend: "no change"},
            {term: {start: 2009, end: 2014}, pct_change: 12, annual: 2.38, trend: "weak increase"}
        ]
    },
    {
        _id: 3,
        Species: "Chaffinch (Fringilla coelebs)",
        population_trends: [
            {term: {start: 1970, end: 2014}, pct_change: 27, annual: 0.55, trend: "no change"},
            {term: {start: 2009, end: 2014}, pct_change: -7, annual: -1.49, trend: "weak decline"}
        ]
    },
    {
        _id: 4,
        Species: "Song Thrush (Turdus philomelos)",
        population_trends: [
            {term: {start: 1970, end: 2014}, pct_change: -53, annual: -1.7, trend: "weak decline"},
            {term: {start: 2009, end: 2014}, pct_change: -4, annual: -0.88, trend: "no change"}
        ]
    }
];

assert.commandWorked(coll.insert(testDocs));

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

if (!FixtureHelpers.isMongos(db)) {
    // Test that if runtimeConstants and let are both specified, both will coexist.
    // Runtime constants are not allowed on mongos passthroughs.
    let constants = {
        localNow: new Date(),
        clusterTime: new Timestamp(0, 0),
    };

    assert.eq(coll.aggregate(pipeline,
                             {runtimeConstants: constants, let : {target_trend: "weak decline"}})
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
}

// Test that $project stage can use 'let' variables
assert.eq(db.runCommand({
                aggregate: coll.getName(),
                pipeline: [
                    {
                        $project: {
                            "var": {
                                $let: {
                                    vars: {variable: "INNER"},
                                    "in": {
                                        $cond: {
                                            "if": {$eq: [{$substr: ["$Species", 0, 1]}, "B"]},
                                            then: "$$variable",
                                            "else": "---"
                                        }
                                    }
                                }
                            }
                        }
                    },
                    {$match: {$expr: {$eq: ["$var", "INNER"]}}}
                ],
                cursor: {}
            }).cursor.firstBatch.length,
          2);

// Test that $project stage can access command-level 'let' variables.
assert.eq(db.runCommand({
                aggregate: coll.getName(),
                pipeline: [
                    {
                        $project: {
                            "var": {
                                $cond: {
                                    "if": {$eq: [{$substr: ["$Species", 0, 1]}, "B"]},
                                    then: "$$variable",
                                    "else": "---"
                                }
                            }
                        }
                    },
                    {$match: {$expr: {$eq: ["$var", "OUTER"]}}}
                ],
                cursor: {},
                "let": {variable: "OUTER"}
            }).cursor.firstBatch.length,
          2);

// Test that $project stage can use stage-level and command-level 'let' variables in same command.
assert.eq(db.runCommand({
                aggregate: coll.getName(),
                pipeline: [
                    {
                        $project: {
                            "var": {
                                $let: {
                                    vars: {innerVar: "INNER"},
                                    "in": {
                                        $cond: {
                                            "if": {$eq: [{$substr: ["$Species", 0, 1]}, "B"]},
                                            then: {$concat: ["$$innerVar", "$$outerVar"]},
                                            "else": "---"
                                        }
                                    }
                                }
                            }
                        }
                    },
                    {$match: {$expr: {$eq: ["$var", "INNEROUTER"]}}}
                ],
                cursor: {},
                "let": {outerVar: "OUTER"}
            }).cursor.firstBatch.length,
          2);

// Test that $project stage follows variable scoping rules with stage-level and command-level 'let'
// variables.
assert.eq(db.runCommand({
                aggregate: coll.getName(),
                pipeline: [
                    {
                        $project: {
                            "var": {
                                $let: {
                                    vars: {variable: "INNER"},
                                    "in": {
                                        $cond: {
                                            "if": {$eq: [{$substr: ["$Species", 0, 1]}, "B"]},
                                            then: "$$variable",
                                            "else": "---"
                                        }
                                    }
                                }
                            }
                        }
                    },
                    {$match: {$expr: {$eq: ["$var", "INNER"]}}}
                ],
                cursor: {},
                "let": {variable: "OUTER"}
            }).cursor.firstBatch.length,
          2);

// Test that the find command works correctly with a let parameter argument.
let result = assert
                 .commandWorked(db.runCommand({
                     find: coll.getName(),
                     let : {target_species: "Song Thrush (Turdus philomelos)"},
                     filter: {$expr: {$eq: ["$Species", "$$target_species"]}},
                     projection: {_id: 0}
                 }))
                 .cursor.firstBatch;
expectedResults = {
    Species: "Song Thrush (Turdus philomelos)",
    population_trends: [
        {term: {start: 1970, end: 2014}, pct_change: -53, annual: -1.7, trend: "weak decline"},
        {term: {start: 2009, end: 2014}, pct_change: -4, annual: -0.88, trend: "no change"}
    ]
};
assert.eq(result.length, 1);
assert.eq(expectedResults, result[0]);

// Delete tests with let params will delete a record, assert that a point-wise find yields an empty
// result, and then restore the collection state for further tests down the line. We can't exercise
// a multi-delete here (limit: 0) because of failures in sharded txn passthrough tests.
assert.commandWorked(db.runCommand({
    delete: coll.getName(),
    let : {target_species: "Song Thrush (Turdus philomelos)"},
    deletes: [{q: {$and: [{_id: 4}, {$expr: {$eq: ["$Species", "$$target_species"]}}]}, limit: 1}]
}));

assert.eq(db.runCommand({find: coll.getName(), filter: {$expr: {$eq: ["$_id", "4"]}}})
              .cursor.firstBatch.length,
          0);
}());
