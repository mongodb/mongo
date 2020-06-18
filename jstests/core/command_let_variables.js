// Tests that commands like find, aggregate and update accepts a 'let' parameter which defines
// variables for use in expressions within the command.
// @tags: [requires_fcv_46]
//
(function() {
"use strict";

load("jstests/libs/fixture_helpers.js");  // For 'isMongos' and 'isSharded'.

const testDB = db.getSiblingDB("command_let_variables");
const coll = testDB.command_let_variables;
const targetColl = testDB.command_let_variables_target;

assert.commandWorked(testDB.dropDatabase());

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

if (!FixtureHelpers.isMongos(testDB)) {
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
    assert.commandFailedWithCode(testDB.runCommand({
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

    assert.commandFailedWithCode(testDB.runCommand({
        aggregate: coll.getName(),
        pipeline: pipeline_no_lets,
        runtimeConstants: constants,
        cursor: {},
        let : null
    }),
                                 ErrorCodes.TypeMismatch);
}

// Test that $project stage can use 'let' variables
assert.eq(testDB
              .runCommand({
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
              })
              .cursor.firstBatch.length,
          2);

// Test that $project stage can access command-level 'let' variables.
assert.eq(testDB
              .runCommand({
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
              })
              .cursor.firstBatch.length,
          2);

// Test that $project stage can use stage-level and command-level 'let' variables in same command.
assert.eq(testDB
              .runCommand({
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
              })
              .cursor.firstBatch.length,
          2);

// Test that $project stage follows variable scoping rules with stage-level and command-level 'let'
// variables.
assert.eq(testDB
              .runCommand({
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
              })
              .cursor.firstBatch.length,
          2);

// Test that the find command works correctly with a let parameter argument.
let result = assert
                 .commandWorked(testDB.runCommand({
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
assert.commandWorked(testDB.runCommand({
    delete: coll.getName(),
    let : {target_species: "Song Thrush (Turdus philomelos)"},
    deletes: [{q: {$and: [{_id: 4}, {$expr: {$eq: ["$Species", "$$target_species"]}}]}, limit: 1}]
}));

result = assert
             .commandWorked(
                 testDB.runCommand({find: coll.getName(), filter: {$expr: {$eq: ["$_id", "4"]}}}))
             .cursor.firstBatch;
assert.eq(result.length, 0);

// Test that reserved names are not allowed as let variable names.
assert.commandFailedWithCode(
    testDB.runCommand(
        {aggregate: coll.getName(), pipeline: [], cursor: {}, let : {Reserved: "failure"}}),
    16867);
assert.commandFailedWithCode(
    testDB.runCommand(
        {aggregate: coll.getName(), pipeline: [], cursor: {}, let : {NOW: "failure"}}),
    16867);
assert.commandFailedWithCode(
    testDB.runCommand(
        {aggregate: coll.getName(), pipeline: [], cursor: {}, let : {CLUSTER_TIME: "failure"}}),
    16867);
assert.commandFailedWithCode(
    testDB.runCommand(
        {aggregate: coll.getName(), pipeline: [], cursor: {}, let : {IS_MR: "failure"}}),
    16867);
assert.commandFailedWithCode(
    testDB.runCommand(
        {aggregate: coll.getName(), pipeline: [], cursor: {}, let : {JS_SCOPE: "failure"}}),
    16867);
assert.commandFailedWithCode(
    testDB.runCommand(
        {aggregate: coll.getName(), pipeline: [], cursor: {}, let : {ROOT: "failure"}}),
    16867);
assert.commandFailedWithCode(
    testDB.runCommand(
        {aggregate: coll.getName(), pipeline: [], cursor: {}, let : {REMOVE: "failure"}}),
    16867);

// Test that let variables can be used within views.
assert.commandWorked(testDB.runCommand({
    create: "core-viewColl",
    viewOn: coll.getName(),
    pipeline: [{$match: {Species: "Song Thrush (Turdus philomelos)"}}]
}));
assert.commandWorked(testDB.runCommand({
    aggregate: "core-viewColl",
    pipeline: [{$addFields: {var : "$$variable"}}],
    let : {variable: "Song Thrush"},
    cursor: {}
}));

// Test that findAndModify works correctly with let parameter arguments.
assert.commandWorked(coll.insert({_id: 5, Species: "spy_bird"}));
result = testDB.runCommand({
    findAndModify: coll.getName(),
    let : {target_species: "spy_bird"},
    // Querying on _id field for sharded collection passthroughs.
    query: {$and: [{_id: 5}, {$expr: {$eq: ["$Species", "$$target_species"]}}]},
    update: {Species: "questionable_bird"},
    new: true
});
expectedResults = {
    _id: 5,
    Species: "questionable_bird"
};
assert.eq(expectedResults, result.value, result);

result = testDB.runCommand({
    findAndModify: coll.getName(),
    let : {species_name: "not_a_bird", realSpecies: "dino"},
    // Querying on _id field for sharded collection passthroughs.
    query: {$and: [{_id: 5}, {$expr: {$eq: ["$Species", "questionable_bird"]}}]},
    update: [{$project: {Species: "$$species_name"}}, {$addFields: {suspect: "$$realSpecies"}}],
    new: true
});
expectedResults = {
    _id: 5,
    Species: "not_a_bird",
    suspect: "dino"
};
assert.eq(expectedResults, result.value, result);

// Test that update respects different parameters in both the query and update part.
result = assert.commandWorked(testDB.runCommand({
    update: coll.getName(),
    updates: [
        {q: {$expr: {$eq: ["$Species", "$$target_species"]}}, u: [{$set: {Species: "$$new_name"}}]}
    ],
    let : {target_species: "Chaffinch (Fringilla coelebs)", new_name: "Chaffinch"}
}));
assert.eq(result.n, 1);
assert.eq(result.nModified, 1);

result = assert.commandWorked(testDB.runCommand(
    {find: coll.getName(), filter: {$expr: {$eq: ["$Species", "Chaffinch (Fringilla coelebs)"]}}}));
assert.eq(result.cursor.firstBatch.length, 0);

result = assert.commandWorked(
    testDB.runCommand({find: coll.getName(), filter: {$expr: {$eq: ["$Species", "Chaffinch"]}}}));
assert.eq(result.cursor.firstBatch.length, 1);

// Test that update respects runtime constants and parameters.
result = assert.commandWorked(testDB.runCommand({
    update: coll.getName(),
    updates: [{
        q: {$expr: {$eq: ["$Species", "$$target_species"]}},
        u: [{$set: {Timestamp: "$$NOW"}}, {$set: {Species: "$$new_name"}}]
    }],
    let : {target_species: "Chaffinch", new_name: "Pied Piper"}
}));
assert.eq(result.n, 1);
assert.eq(result.nModified, 1);

result = assert.commandWorked(
    testDB.runCommand({find: coll.getName(), filter: {$expr: {$eq: ["$Species", "Chaffinch"]}}}));
assert.eq(result.cursor.firstBatch.length, 0, result);

result = assert.commandWorked(
    testDB.runCommand({find: coll.getName(), filter: {$expr: {$eq: ["$Species", "Pied Piper"]}}}));
assert.eq(result.cursor.firstBatch.length, 1, result);

// Test that undefined let params in the update's query part fail gracefully.
assert.commandFailedWithCode(testDB.runCommand({
    update: coll.getName(),
    updates: [{
        q: {$expr: {$eq: ["$Species", "$$target_species"]}},
        u: [{$set: {Species: "Homo Erectus"}}]
    }],
    let : {cat: "not_a_bird"}
}),
                             17276);

// Test that undefined let params in the update's update part fail gracefully.
assert.commandFailedWithCode(testDB.runCommand({
    update: coll.getName(),
    updates: [{
        q: {$expr: {$eq: ["$Species", "Chaffinch (Fringilla coelebs)"]}},
        u: [{$set: {Species: "$$new_name"}}]
    }],
    let : {cat: "not_a_bird"}
}),
                             17276);
}());
