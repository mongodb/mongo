/**
 * Tests that using a $graphLookup stage inside of a $facet stage will yield the same results as
 * using the $graphLookup stage outside of the $facet stage.
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For documentEq.

// We will only use one collection, the $graphLookup will look up from the same collection.
var graphColl = db.facetGraphLookup;

// The graph in ASCII form: 0 --- 1 --- 2    3
graphColl.drop();
assert.commandWorked(graphColl.insert({_id: 0, edges: [1]}));
assert.commandWorked(graphColl.insert({_id: 1, edges: [0, 2]}));
assert.commandWorked(graphColl.insert({_id: 2, edges: [1]}));
assert.commandWorked(graphColl.insert({_id: 3}));

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

const projectStage = {
    $project: {_id: 1, edges: 1, connected_length: {$size: "$connected"}}
};

const normalResults = graphColl.aggregate([graphLookupStage, projectStage]).toArray();
const facetedResults =
    graphColl.aggregate([{$facet: {nested: [graphLookupStage, projectStage]}}]).toArray();
arrayEq(facetedResults, [{nested: normalResults}]);

const sortStage = {
    $sort: {_id: 1, "connected._id": 1}
};

const normalResultsUnwound =
    graphColl.aggregate([graphLookupStage, {$unwind: "$connected"}, sortStage]).toArray();
const facetedResultsUnwound =
    graphColl
        .aggregate([{$facet: {nested: [graphLookupStage, {$unwind: "$connected"}, sortStage]}}])
        .toArray();
arrayEq(facetedResultsUnwound, [{nested: normalResultsUnwound}]);
}());
