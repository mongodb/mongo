/**
 * Tests that using a $lookup stage inside of a $facet stage will yield the same results as using
 * the $lookup stage outside of the $facet stage.
 */
(function() {
"use strict";

var local = db.facetLookupLocal;
var foreign = db.facetLookupForeign;

local.drop();
assert.commandWorked(local.insert({_id: 0}));
assert.commandWorked(local.insert({_id: 1}));

foreign.drop();
assert.commandWorked(foreign.insert({_id: 0, foreignKey: 0}));
assert.commandWorked(foreign.insert({_id: 1, foreignKey: 1}));
assert.commandWorked(foreign.insert({_id: 2, foreignKey: 2}));

function runTest(lookupStage) {
    const lookupResults = local.aggregate([lookupStage]).toArray();
    const facetedLookupResults = local.aggregate([{$facet: {nested: [lookupStage]}}]).toArray();
    assert.eq(facetedLookupResults, [{nested: lookupResults}]);

    const lookupResultsUnwound = local.aggregate([lookupStage, {$unwind: "$joined"}]).toArray();
    const facetedLookupResultsUnwound =
        local.aggregate([{$facet: {nested: [lookupStage, {$unwind: "$joined"}]}}]).toArray();
    assert.eq(facetedLookupResultsUnwound, [{nested: lookupResultsUnwound}]);
}

runTest({
    $lookup: {from: foreign.getName(), localField: "_id", foreignField: "foreignKey", as: "joined"}
});

runTest({
        $lookup: {
            from: foreign.getName(),
            let : {id1: "$_id"},
            pipeline: [
                {$match: {$expr: {$eq: ["$$id1", "$foreignKey"]}}},
                {
                  $lookup: {
                      from: foreign.getName(),
                      let : {id2: "$_id"},
                      pipeline: [{$match: {$expr: {$eq: ["$$id2", "$foreignKey"]}}}],
                      as: "joined2"
                  }
                },
                {$unwind: "$joined2"}
            ],
            as: "joined"
        }
    });
}());
