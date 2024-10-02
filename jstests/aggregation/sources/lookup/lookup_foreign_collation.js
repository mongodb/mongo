/**
 * Tests that $lookup respects an internally-specified foreign pipeline collation.
 * @tags: [
 *   do_not_wrap_aggregations_in_facets,
 *   requires_pipeline_optimization,
 * ]
 */
import {anyEq} from "jstests/aggregation/extras/utils.js";
import {getSingleNodeExplain} from "jstests/libs/analyze_plan.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

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
    //    expectedResults: Results expected from the aggregate invocation
    //
    function assertExpectedResultSet(localColl, foreignColl, commandCollation, expectedResults) {
        const lookupWithPipeline = {$lookup: {from: foreignColl.getName(),
                                 as: "foreignMatch",
                                 let: {l_id: "$_id"},
                                 pipeline: [{$match: {$expr: {$eq: ["$_id", "$$l_id"]}}}]}};

        const lookupWithLocalForeignField = {$lookup: {from: foreignColl.getName(),
         localField: "_id",
         foreignField: "_id",
         as: "foreignMatch"}};

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
    // the command or the collection.
    assertExpectedResultSet(localColl, foreignColl, null, resultSetCaseSensitive);

    // When a collation has been specified on the $lookup stage, it will always be used to join
    // local and foreign collections.
    for (const local of [localColl, localCaseInsensitiveColl]) {
        for (const foreign of [foreignColl, foreignCaseInsensitiveColl]) {
            // Case insensitive collation results in a case insensitive join.
            assertExpectedResultSet(
                local, foreign, caseInsensitiveCollation, resultSetCaseInsensitive);

            // Simple collation results in a case sensitive join.
            assertExpectedResultSet(local, foreign, simpleCollation, resultSetCaseSensitive);
        }
    }
})();