// Tests that commands like find, aggregate and update accepts a 'let' parameter which defines
// variables for use in expressions within the command.
// @tags: [
// ]
//
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");
load("jstests/libs/fixture_helpers.js");  // For 'isMongos' and 'isSharded'.

const testDB = db.getSiblingDB("command_let_variables");
const coll = testDB.command_let_variables;
coll.drop();

const isMongos = FixtureHelpers.isMongos(testDB);
const isCollSharded = FixtureHelpers.isSharded(coll);

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

// Test that running explain on the agg command works as expected.
let explain = assert.commandWorked(testDB.runCommand({
    explain:
        {aggregate: coll.getName(), pipeline, let : {target_trend: "weak decline"}, cursor: {}},
    verbosity: "executionStats"
}));
if (!isMongos) {
    assert(explain.hasOwnProperty("stages"), explain);
    assert.neq(explain.stages.length, 0, explain);
    let lastStage = explain.stages[explain.stages.length - 1];
    assert.eq(lastStage.nReturned, 2, explain);
}

if (!isMongos) {
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

    assert.commandWorked(testDB.runCommand({
        aggregate: coll.getName(),
        pipeline: pipeline_no_lets,
        runtimeConstants: constants,
        cursor: {},
        let : null
    }));

    assert.commandFailedWithCode(testDB.runCommand({
        aggregate: coll.getName(),
        pipeline: pipeline_no_lets,
        runtimeConstants: constants,
        cursor: {},
        let : 1
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

// Test that let parameters work as expected when the find is run as an explain.
explain = assert.commandWorked(testDB.runCommand({
    explain: {
        find: coll.getName(),
        let : {target_species: "Song Thrush (Turdus philomelos)"},
        filter: {$expr: {$eq: ["$Species", "$$target_species"]}},
        projection: {_id: 0}
    },
    verbosity: "executionStats"
}));
if (!isMongos) {
    assert.eq(explain.executionStats.nReturned, 1, explain);
}

// Delete tests with let params will delete a record, assert that a point-wise find yields an
// empty result, and then restore the collection state for further tests down the line. We can't
// exercise a multi-delete here (limit: 0) because of failures in sharded txn passthrough tests.
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

assert.commandWorked(coll.insert({_id: 4, Species: "bird_to_remove"}));

// Test that explain of a delete command works as expected with 'let' parameters.
explain = assert.commandWorked(testDB.runCommand({
    explain: {
        delete: coll.getName(),
        let : {target_species: "bird_to_remove"},
        deletes:
            [{q: {$and: [{_id: 4}, {$expr: {$eq: ["$Species", "$$target_species"]}}]}, limit: 1}]
    },
    verbosity: "executionStats"
}));
if (!isMongos) {
    let deleteStage = getPlanStage(explain.executionStats.executionStages, "DELETE");
    assert.eq(deleteStage.nWouldDelete, 1, explain);
}

// Test that the .remove() shell helper supports let parameters.
result = assert.commandWorked(
    coll.remove({$and: [{_id: 4}, {$expr: {$eq: ["$Species", "$$target_species"]}}]},
                {justOne: true, let : {target_species: "bird_to_remove"}}));
assert.eq(result.nRemoved, 1);

result = assert
             .commandWorked(
                 testDB.runCommand({find: coll.getName(), filter: {$expr: {$eq: ["$_id", "4"]}}}))
             .cursor.firstBatch;
assert.eq(result.length, 0);

// Test that reserved names are not allowed as let variable names.
assert.commandFailedWithCode(
    testDB.runCommand(
        {aggregate: coll.getName(), pipeline: [], cursor: {}, let : {Reserved: "failure"}}),
    ErrorCodes.FailedToParse);
assert.commandFailedWithCode(
    testDB.runCommand(
        {aggregate: coll.getName(), pipeline: [], cursor: {}, let : {NOW: "failure"}}),
    4738901);
assert.commandFailedWithCode(
    testDB.runCommand(
        {aggregate: coll.getName(), pipeline: [], cursor: {}, let : {CLUSTER_TIME: "failure"}}),
    4738901);
assert.commandFailedWithCode(
    testDB.runCommand(
        {aggregate: coll.getName(), pipeline: [], cursor: {}, let : {IS_MR: "failure"}}),
    4738901);
assert.commandFailedWithCode(
    testDB.runCommand(
        {aggregate: coll.getName(), pipeline: [], cursor: {}, let : {JS_SCOPE: "failure"}}),
    4738901);
assert.commandFailedWithCode(
    testDB.runCommand(
        {aggregate: coll.getName(), pipeline: [], cursor: {}, let : {ROOT: "failure"}}),
    ErrorCodes.FailedToParse);
assert.commandFailedWithCode(
    testDB.runCommand(
        {aggregate: coll.getName(), pipeline: [], cursor: {}, let : {REMOVE: "failure"}}),
    ErrorCodes.FailedToParse);

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

assert.commandWorked(coll.insert({_id: 5, Species: "spy_bird"}));

// Test that explain of findAndModify works correctly with let parameters.
explain = assert.commandWorked(testDB.runCommand({
    explain: {
        findAndModify: coll.getName(),
        let : {target_species: "spy_bird"},
        // Querying on _id field for sharded collection passthroughs.
        query: {$and: [{_id: 5}, {$expr: {$eq: ["$Species", "$$target_species"]}}]},
        update: {Species: "questionable_bird"},
        new: true
    },
    verbosity: "executionStats"
}));
if (!isMongos) {
    let updateStage = getPlanStage(explain.executionStats.executionStages, "UPDATE");
    assert.eq(updateStage.nMatched, 1, explain);
    assert.eq(updateStage.nWouldModify, 1, explain);
}

// Test that findAndModify works correctly with let parameter arguments.
result = assert.commandWorked(testDB.runCommand({
    findAndModify: coll.getName(),
    let : {target_species: "spy_bird"},
    // Querying on _id field for sharded collection passthroughs.
    query: {$and: [{_id: 5}, {$expr: {$eq: ["$Species", "$$target_species"]}}]},
    update: {Species: "questionable_bird"},
    new: true
}));
expectedResults = {
    _id: 5,
    Species: "questionable_bird"
};
assert.eq(expectedResults, result.value, result);

result = assert.commandWorked(testDB.runCommand({
    findAndModify: coll.getName(),
    let : {species_name: "not_a_bird", realSpecies: "dino"},
    // Querying on _id field for sharded collection passthroughs.
    query: {$and: [{_id: 5}, {$expr: {$eq: ["$Species", "questionable_bird"]}}]},
    update: [{$project: {Species: "$$species_name"}}, {$addFields: {suspect: "$$realSpecies"}}],
    new: true
}));
expectedResults = {
    _id: 5,
    Species: "not_a_bird",
    suspect: "dino"
};
assert.eq(expectedResults, result.value, result);

// Test that explain of update works correctly with let parameters.
explain = assert.commandWorked(testDB.runCommand({
    explain: {
        update: coll.getName(),
        updates: [{
            q: {_id: 3, $expr: {$eq: ["$Species", "$$target_species"]}},
            u: [{$set: {Species: "$$new_name"}}],
        }],
        let : {target_species: "Chaffinch (Fringilla coelebs)", new_name: "Chaffinch"}
    },
    verbosity: "executionStats"
}));
if (!isMongos) {
    let updateStage = getPlanStage(explain.executionStats.executionStages, "UPDATE");
    assert.eq(updateStage.nMatched, 1, explain);
    assert.eq(updateStage.nWouldModify, 1, explain);
}

// Test that update respects different parameters in both the query and update part.
result = assert.commandWorked(testDB.runCommand({
    update: coll.getName(),
    updates: [{
        q: {_id: 3, $expr: {$eq: ["$Species", "$$target_species"]}},
        u: [{$set: {Species: "$$new_name"}}],
    }],
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
        q: {_id: 3, $expr: {$eq: ["$Species", "$$target_species"]}},
        u: [{$set: {Timestamp: "$$NOW"}}, {$set: {Species: "$$new_name"}}],
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

// This forces a multi-statement transaction to commit if this test is running in one of the
// multi-statement transaction passthrough suites. We need to do this to ensure the updates
// above commit before running an update that will fail, as the failed update aborts the entire
// transaction and rolls back the updates above.
assert.commandWorked(testDB.runCommand({ping: 1}));

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
        q: {_id: 3, $expr: {$eq: ["$Species", "Chaffinch (Fringilla coelebs)"]}},
        u: [{$set: {Species: "$$new_name"}}],
    }],
    let : {cat: "not_a_bird"}
}),
                             17276);

// Test that the .update() shell helper supports let parameters.
result = assert.commandWorked(
    coll.update({_id: 3, $expr: {$eq: ["$Species", "$$target_species"]}},
                [{$set: {Species: "$$new_name"}}],
                {let : {target_species: "Pied Piper", new_name: "Chaffinch"}}));
assert.eq(result.nMatched, 1);
assert.eq(result.nModified, 1);

result = assert.commandWorked(
    testDB.runCommand({find: coll.getName(), filter: {$expr: {$eq: ["$Species", "Pied Piper"]}}}));
assert.eq(result.cursor.firstBatch.length, 0, result);

result = assert.commandWorked(
    testDB.runCommand({find: coll.getName(), filter: {$expr: {$eq: ["$Species", "Chaffinch"]}}}));
assert.eq(result.cursor.firstBatch.length, 1, result);

// Test that let variables can be initialized with an expression.
result = assert
             .commandWorked(testDB.runCommand({
                 find: coll.getName(),
                 filter: {},
                 projection: {_id: "$$a"},
                 limit: 1,
                 let : {a: {$add: [2, 3]}},
             }))
             .cursor.firstBatch;
assert.eq(result, [{_id: 5}]);

// Test that the expression cannot refer to any fields or variables.
assert.commandFailedWithCode(testDB.runCommand({
    find: coll.getName(),
    filter: {},
    let : {a: "$_id"},
}),
                             4890500);
assert.commandFailedWithCode(testDB.runCommand({
    find: coll.getName(),
    filter: {},
    let : {a: "$$oops_undefined_var"},
}),
                             17276);

// Test that each expression can refer to previously defined let variables.
result = assert
             .commandWorked(testDB.runCommand({
                 find: coll.getName(),
                 filter: {},
                 projection: {_id: "$$c"},
                 limit: 1,
                 let : {
                     a: 2,
                     b: {$add: ["$$a", 3]},
                     c: {$multiply: ["$$a", "$$b"]},
                 }
             }))
             .cursor.firstBatch;
assert.eq([{_id: (2 + 3) * 2}], result);

// Test that $rand is allowed in the initializer.
result = assert
             .commandWorked(testDB.runCommand({
                 find: coll.getName(),
                 filter: {},
                 projection: {_id: "$$a"},
                 limit: 1,
                 let : {a: {$rand: {}}},
             }))
             .cursor.firstBatch[0]
             ._id;
assert.between(0, result, 1);

// Test that each initializer expression is evaluated separately.
{
    const values = assert
                       .commandWorked(testDB.runCommand({
                           find: coll.getName(),
                           filter: {},
                           projection: {_id: ["$$a", "$$b", "$$c", "$$d"]},
                           limit: 1,
                           let : {
                               a: {$rand: {}},
                               b: {$rand: {}},
                               c: {$rand: {}},
                               d: {$rand: {}},
                           }
                       }))
                       .cursor.firstBatch[0]
                       ._id;

    const deduped = [...new Set(values)];
    assert.eq(values.length, deduped.length, `Expected all distinct values: ${values}`);
}

// Test that the expressions are evaluated once up front.
//
// TODO SERVER-75927: This does not work as expected when the collection is sharded. Once the bug
// is fixed, we should re-enable this test case when the collection is sharded.
if (!isCollSharded) {
    const values = assert
                       .commandWorked(testDB.runCommand({
                           find: coll.getName(),
                           filter: {},
                           projection: {_id: "$$a"},
                           let : {a: {$rand: {}}},
                       }))
                       .cursor.firstBatch.map(doc => doc._id);
    assert.gt(values.length, 1);

    const deduped = [...new Set(values)];
    assert.eq(1, deduped.length, `Expected all identical values: ${deduped}`);
}

// Test that expressions wrapped with $literal are serialized correctly when run in sharded cluster
// environments.
result = coll.aggregate([{$match: {$expr: {$eq: ["$_id", 2]}}}, {$project: {a: "$$b"}}],
                        {let : {b: {$literal: "$notAFieldPath"}}})
             .toArray();
assert.eq(result, [{_id: 2, a: "$notAFieldPath"}]);
}());
