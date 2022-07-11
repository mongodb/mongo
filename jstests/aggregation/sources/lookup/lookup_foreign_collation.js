/**
 * Tests that $lookup respects an internally-specified foreign pipeline collation.
 * @tags: [
 *   do_not_wrap_aggregations_in_facets,
 *   requires_pipeline_optimization,
 * ]
 */
load("jstests/aggregation/extras/utils.js");  // For anyEq.

(function() {

"use strict";

const testDB = db.getSiblingDB(jsTestName());
const localColl = testDB.local_no_collation;
const localCaseInsensitiveColl = testDB.local_collation;
const foreignColl = testDB.foreign_no_collation;
const foreignCaseInsensitiveColl = testDB.foreign_collation;

const caseInsensitiveCollation = {
    locale: "en_US",
    strength: 1
};

const simpleCollation = {
    locale: "simple"
};

function setup() {
    assert.commandWorked(testDB.runCommand({dropDatabase: 1}));

    assert.commandWorked(testDB.createCollection(localColl.getName()));
    assert.commandWorked(testDB.createCollection(foreignColl.getName()));
    assert.commandWorked(testDB.createCollection(localCaseInsensitiveColl.getName(),
                                                 {collation: caseInsensitiveCollation}));
    assert.commandWorked(testDB.createCollection(foreignCaseInsensitiveColl.getName(),
                                                 {collation: caseInsensitiveCollation}));

    assert.commandWorked(
        localColl.insert([{_id: "a"}, {_id: "b"}, {_id: "c"}, {_id: "d"}, {_id: "e"}]));
    assert.commandWorked(localCaseInsensitiveColl.insert(
        [{_id: "a"}, {_id: "b"}, {_id: "c"}, {_id: "d"}, {_id: "e"}]));
    assert.commandWorked(
        foreignColl.insert([{_id: "a"}, {_id: "B"}, {_id: "c"}, {_id: "D"}, {_id: "e"}]));
    assert.commandWorked(foreignCaseInsensitiveColl.insert(
        [{_id: "a"}, {_id: "B"}, {_id: "c"}, {_id: "D"}, {_id: "e"}]));
}

(function testCollationPermutations() {
    setup();

    // Pipeline style $lookup with cases insensitive collation.
    const lookupWithPipeline = (foreignColl) => {
        return {
          $lookup: {from: foreignColl.getName(), as: "foreignMatch", _internalCollation: caseInsensitiveCollation, let: {l_id: "$_id"}, pipeline: [{$match: {$expr: {$eq: ["$_id", "$$l_id"]}}}]}
      };
    };

    // Local-field foreign-field style $lookup with cases insensitive collation.
    const lookupWithLocalForeignField = (foreignColl) => {
        return {
          $lookup: {from: foreignColl.getName(), localField: "_id", foreignField: "_id", as: "foreignMatch", _internalCollation: caseInsensitiveCollation}
      };
    };

    const resultSetCaseInsensitive = [
        {_id: "a", foreignMatch: [{_id: "a"}]},
        {_id: "b", foreignMatch: [{_id: "B"}]},
        {_id: "c", foreignMatch: [{_id: "c"}]},
        {_id: "d", foreignMatch: [{_id: "D"}]},
        {_id: "e", foreignMatch: [{_id: "e"}]}
    ];

    const resultSetCaseSensitive = [
        {_id: "a", foreignMatch: [{_id: "a"}]},
        {_id: "b", foreignMatch: []},
        {_id: "c", foreignMatch: [{_id: "c"}]},
        {_id: "d", foreignMatch: []},
        {_id: "e", foreignMatch: [{_id: "e"}]}
    ];

    // Executes an aggregation pipeline with both pipeline and localField/foreignField $lookup
    // syntax, exercising different combinations of collation setting. Asserts that the results
    // returned match those expected. Arguments:
    //    localColl: Local Collection
    //    foreignColl: Foreign Collection
    //    commandCollation: Collation set on the aggregate command. Pass null for default collation.
    //    lookupCollation: Collation specified in the $lookup stage.  Pass null for default
    //    collation. expectedResults: Results expected from the aggregate invocation
    //
    function assertExpectedResultSet(
        localColl, foreignColl, commandCollation, lookupCollation, expectedResults) {
        const lookupWithPipeline = {$lookup: {from: foreignColl.getName(), 
                                 as: "foreignMatch",
                                 let: {l_id: "$_id"}, 
                                 pipeline: [{$match: {$expr: {$eq: ["$_id", "$$l_id"]}}}]}};

        const lookupWithLocalForeignField = {$lookup: {from: foreignColl.getName(), 
         localField: "_id", 
         foreignField: "_id", 
         as: "foreignMatch"}};

        if (lookupCollation) {
            lookupWithPipeline.$lookup._internalCollation = lookupCollation;
            lookupWithLocalForeignField.$lookup._internalCollation = lookupCollation;
        }

        const aggOptions = {};
        if (commandCollation) {
            aggOptions.collation = commandCollation;
        }

        let results = localColl.aggregate([lookupWithPipeline], aggOptions).toArray();
        assert(anyEq(results, expectedResults), tojson(results));

        results = localColl.aggregate([lookupWithLocalForeignField], aggOptions).toArray();
        assert(anyEq(results, expectedResults), tojson(results));
    }

    // Baseline test, confirming simple binary comparison when no collation has been specified on
    // the command, $lookup stage or collections.
    assertExpectedResultSet(localColl, foreignColl, null, null, resultSetCaseSensitive);

    // When a collation has been specified on the $lookup stage, it will always be used to join
    // local and foreign collections.
    for (const local of [localColl, localCaseInsensitiveColl]) {
        for (const foreign of [foreignColl, foreignCaseInsensitiveColl]) {
            for (const command of [null, simpleCollation, caseInsensitiveCollation]) {
                // Case insensitive collation specified in the $lookup stage results in a case
                // insensitive join.
                assertExpectedResultSet(
                    local, foreign, command, caseInsensitiveCollation, resultSetCaseInsensitive);

                // Simple collation specified in the $lookup stage results in a case sensitive join.
                assertExpectedResultSet(
                    local, foreign, command, simpleCollation, resultSetCaseSensitive);
            }
        }
    }
})();

(function testNestedLookupStagesWithDifferentCollations() {
    setup();

    const lookupWithPipeline = {$lookup: {from: foreignColl.getName(), 
         as: "foreignMatch",
         let: {l_id: "$_id"}, 
         pipeline: [{$match: {$expr: {$eq: ["$_id", "$$l_id"]}}},
                      {$lookup: {from: localColl.getName(),
                         as: "foreignMatch2",
                         let: {l_id: "$_id"},
                         pipeline: [{$match: {$expr: {$eq: ["$_id", "$$l_id"]}}}],
                         _internalCollation: simpleCollation}}],
         _internalCollation: caseInsensitiveCollation}};

    const resultSet = [
        {_id: "a", foreignMatch: [{_id: "a", "foreignMatch2": [{"_id": "a"}]}]},
        {_id: "b", foreignMatch: [{_id: "B", "foreignMatch2": []}]},
        {_id: "c", foreignMatch: [{_id: "c", "foreignMatch2": [{"_id": "c"}]}]},
        {_id: "d", foreignMatch: [{_id: "D", "foreignMatch2": []}]},
        {_id: "e", foreignMatch: [{_id: "e", "foreignMatch2": [{"_id": "e"}]}]}
    ];

    const results = localColl.aggregate([lookupWithPipeline]).toArray();
    assert(anyEq(results, resultSet), tojson(results));
})();

(function testMatchOnUnwoundAsFieldAbsorptionOptimization() {
    setup();

    // A $lookup stage with a collation that differs from the collection and command collation
    // will not absorb a $match on unwound results.
    let pipeline = [{$lookup: {from: foreignColl.getName(), 
         as: "foreignMatch",
         let: {l_id: "$_id"}, 
         pipeline: [{$match: {$expr: {$eq: ["$_id", "$$l_id"]}}}],
         _internalCollation: caseInsensitiveCollation}},
         {$unwind: "$foreignMatch"},
         {$match: {"foreignMatch._id": "b"}}];

    let results = localColl.aggregate(pipeline).toArray();
    assert.eq(0, results.length);

    let explain = localColl.explain().aggregate(pipeline);
    let lastStage = explain.stages[explain.stages.length - 1];
    assert(lastStage.hasOwnProperty("$match"), tojson(explain));
    assert.eq({$match: {"foreignMatch._id": {$eq: "b"}}},
              lastStage,
              "The $match stage should not be optimized into the $lookup stage" + tojson(explain));

    // A $lookup stage with a collation that matches the command collation will absorb a $match
    // stage.
    pipeline = [{$lookup: {from: foreignColl.getName(), 
         as: "foreignMatch",
         let: {l_id: "$_id"}, 
         pipeline: [{$match: {$expr: {$eq: ["$_id", "$$l_id"]}}}],
         _internalCollation: caseInsensitiveCollation}},
         {$unwind: "$foreignMatch"},
         {$match: {"foreignMatch._id": "b"}}];

    let expectedResults = [{"_id": "b", "foreignMatch": {"_id": "B"}}];

    results = localColl.aggregate(pipeline, {collation: caseInsensitiveCollation}).toArray();
    assert(anyEq(results, expectedResults), tojson(results));

    explain = localColl.explain().aggregate(pipeline, {collation: caseInsensitiveCollation});
    lastStage = explain.stages[explain.stages.length - 1];
    assert(lastStage.hasOwnProperty("$lookup"), tojson(explain));

    // A $lookup stage with a collation that matches the local collection collation will absorb
    // a $match stage.
    pipeline = [{$lookup: {from: foreignColl.getName(), 
         as: "foreignMatch",
         let: {l_id: "$_id"},
         pipeline: [{$match: {$expr: {$eq: ["$_id", "$$l_id"]}}}],
         _internalCollation: caseInsensitiveCollation}},
         {$unwind: "$foreignMatch"},
         {$match: {"foreignMatch._id": "b"}}];

    expectedResults = [{"_id": "b", "foreignMatch": {"_id": "B"}}];

    results = localCaseInsensitiveColl.aggregate(pipeline).toArray();
    assert(anyEq(results, expectedResults), tojson(results));

    explain = localCaseInsensitiveColl.explain().aggregate(pipeline);
    lastStage = explain.stages[explain.stages.length - 1];
    assert(lastStage.hasOwnProperty("$lookup"), tojson(explain));
})();
})();
