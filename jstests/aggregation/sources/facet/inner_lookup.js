/**
 * Tests that using a $lookup stage inside of a $facet stage will yield the same results as using
 * the $lookup stage outside of the $facet stage.
 */
(function() {
    "use strict";

    var local = db.facetLookupLocal;
    var foreign = db.facetLookupForeign;

    local.drop();
    assert.writeOK(local.insert({_id: 0}));
    assert.writeOK(local.insert({_id: 1}));

    foreign.drop();
    assert.writeOK(foreign.insert({_id: 0, foreignKey: 0}));
    assert.writeOK(foreign.insert({_id: 1, foreignKey: 1}));
    assert.writeOK(foreign.insert({_id: 2, foreignKey: 2}));

    const lookupStage = {
        $lookup:
            {from: foreign.getName(), localField: "_id", foreignField: "foreignKey", as: "joined"}
    };
    const lookupResults = local.aggregate([lookupStage]).toArray();
    const facetedLookupResults = local.aggregate([{$facet: {nested: [lookupStage]}}]).toArray();
    assert.eq(facetedLookupResults, [{nested: lookupResults}]);

    const lookupResultsUnwound = local.aggregate([lookupStage, {$unwind: "$joined"}]).toArray();
    const facetedLookupResultsUnwound =
        local.aggregate([{$facet: {nested: [lookupStage, {$unwind: "$joined"}]}}]).toArray();
    assert.eq(facetedLookupResultsUnwound, [{nested: lookupResultsUnwound}]);
}());
