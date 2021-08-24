/**
 * Tests that $lookup respects the user-specified collation or the inherited local collation
 * when performing comparisons on a foreign collection with a different default collation. Exercises
 * the fix for SERVER-43350.
 */
load("jstests/aggregation/extras/utils.js");  // For anyEq.

(function() {

"use strict";

load("jstests/libs/fixture_helpers.js");  // For isSharded.

const testDB = db.getSiblingDB(jsTestName());
assert.commandWorked(testDB.dropDatabase());

const caseInsensitiveCollation = {
    locale: "en_US",
    strength: 1
};

const simpleCollation = {
    locale: "simple"
};

assert.commandWorked(testDB.createCollection("no_collation"));
assert.commandWorked(
    testDB.createCollection("case_insensitive", {collation: caseInsensitiveCollation}));

const noCollationColl = testDB.no_collation;
const caseInsensitiveColl = testDB.case_insensitive;

// Do not run the rest of the tests if the foreign collection is implicitly sharded but the flag to
// allow $lookup/$graphLookup into a sharded collection is disabled.
const getShardedLookupParam = db.adminCommand({getParameter: 1, featureFlagShardedLookup: 1});
const isShardedLookupEnabled = getShardedLookupParam.hasOwnProperty("featureFlagShardedLookup") &&
    getShardedLookupParam.featureFlagShardedLookup.value;
if (FixtureHelpers.isSharded(caseInsensitiveColl) && !isShardedLookupEnabled) {
    return;
}

assert.commandWorked(
    noCollationColl.insert([{_id: "a"}, {_id: "b"}, {_id: "c"}, {_id: "d"}, {_id: "e"}]));
assert.commandWorked(
    caseInsensitiveColl.insert([{_id: "a"}, {_id: "B"}, {_id: "c"}, {_id: "D"}, {_id: "e"}]));

const lookupWithPipeline = (foreignColl) => {
    return {
        $lookup: {from: foreignColl.getName(), as: "foreignMatch", let: {l_id: "$_id"}, pipeline: [{$match: {$expr: {$eq: ["$_id", "$$l_id"]}}}]}
    };
};
const lookupNoPipeline = (foreignColl) => {
    return {
        $lookup: {from: foreignColl.getName(), localField: "_id", foreignField: "_id", as: "foreignMatch"}
    };
};

for (let lookupInto of [lookupWithPipeline, lookupNoPipeline]) {
    // Verify that a $lookup whose local collection has no default collation uses the simple
    // collation for comparisons on a foreign collection with a non-simple default collation.
    let results = noCollationColl.aggregate([lookupInto(caseInsensitiveColl)]).toArray();
    assert(anyEq(results, [
        {_id: "a", foreignMatch: [{_id: "a"}]},
        {_id: "b", foreignMatch: []},
        {_id: "c", foreignMatch: [{_id: "c"}]},
        {_id: "d", foreignMatch: []},
        {_id: "e", foreignMatch: [{_id: "e"}]}
    ]));

    // Verify that a $lookup whose local collection has no default collation but which is running in
    // a pipeline with a non-simple user-specified collation uses the latter for comparisons on the
    // foreign collection.
    results =
        noCollationColl
            .aggregate([lookupInto(caseInsensitiveColl)], {collation: caseInsensitiveCollation})
            .toArray();
    assert(anyEq(results, [
        {_id: "a", foreignMatch: [{_id: "a"}]},
        {_id: "b", foreignMatch: [{_id: "B"}]},
        {_id: "c", foreignMatch: [{_id: "c"}]},
        {_id: "d", foreignMatch: [{_id: "D"}]},
        {_id: "e", foreignMatch: [{_id: "e"}]}
    ]));

    // Verify that a $lookup whose local collection has a non-simple collation uses the latter for
    // comparisons on a foreign collection with no default collation.
    results = caseInsensitiveColl.aggregate([lookupInto(noCollationColl)]).toArray();
    assert(anyEq(results, [
        {_id: "a", foreignMatch: [{_id: "a"}]},
        {_id: "B", foreignMatch: [{_id: "b"}]},
        {_id: "c", foreignMatch: [{_id: "c"}]},
        {_id: "D", foreignMatch: [{_id: "d"}]},
        {_id: "e", foreignMatch: [{_id: "e"}]}
    ]));

    // Verify that a $lookup whose local collection has a non-simple collation but which is running
    // in a pipeline with a user-specified simple collation uses the latter for comparisons on the
    // foreign collection.
    results =
        caseInsensitiveColl.aggregate([lookupInto(noCollationColl)], {collation: simpleCollation})
            .toArray();
    assert(anyEq(results, [
        {_id: "a", foreignMatch: [{_id: "a"}]},
        {_id: "B", foreignMatch: []},
        {_id: "c", foreignMatch: [{_id: "c"}]},
        {_id: "D", foreignMatch: []},
        {_id: "e", foreignMatch: [{_id: "e"}]}
    ]));
}
})();
