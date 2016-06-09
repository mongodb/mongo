/**
 * Tests that using a $graphLookup stage inside of a $facet stage will yield the same results as
 * using the $graphLookup stage outside of the $facet stage.
 */
(function() {
    "use strict";

    // We will only use one collection, the $graphLookup will look up from the same collection.
    var graphColl = db.facetGraphLookup;

    // The graph in ASCII form: 0 --- 1 --- 2    3
    graphColl.drop();
    assert.writeOK(graphColl.insert({_id: 0, edges: [1]}));
    assert.writeOK(graphColl.insert({_id: 1, edges: [0, 2]}));
    assert.writeOK(graphColl.insert({_id: 2, edges: [1]}));
    assert.writeOK(graphColl.insert({_id: 3}));

    // For each document in the collection, this will compute all the other documents that are
    // reachable from this one.
    const graphLookupStage = {
        $graphLookup: {
            from: graphColl.getName(),
            startWith: "$_id",
            connectFromField: "edges",
            connectToField: "_id",
            as: "connected"
        }
    };
    const normalResults = graphColl.aggregate([graphLookupStage]).toArray();
    const facetedResults = graphColl.aggregate([{$facet: {nested: [graphLookupStage]}}]).toArray();
    assert.eq(facetedResults, [{nested: normalResults}]);

    const normalResultsUnwound =
        graphColl.aggregate([graphLookupStage, {$unwind: "$connected"}]).toArray();
    const facetedResultsUnwound =
        graphColl.aggregate([{$facet: {nested: [graphLookupStage, {$unwind: "$connected"}]}}])
            .toArray();
    assert.eq(facetedResultsUnwound, [{nested: normalResultsUnwound}]);
}());
