/**
 * Test that $graphLookup can run on a view containing a $search or $vectorSearch stage.
 */
import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";
import {
    getAirportData,
    getAirportNameEmbeddingById,
    getAirportSearchIndexSpec,
    getAirportVectorSearchIndexSpec,
} from "jstests/with_mongot/e2e_lib/data/airports.js";

const collName = jsTestName();
const coll = db[collName];
coll.drop();

const airports = getAirportData();
// All airports except LGA are "international".
const internationalAirports = airports.length - 1;
coll.insertMany(airports);

// Set up search and vector search indexes on the airports collection.
const searchIndexSpec = getAirportSearchIndexSpec();
createSearchIndex(coll, searchIndexSpec);

const vectorSearchIndexSpec = getAirportVectorSearchIndexSpec();
createSearchIndex(coll, vectorSearchIndexSpec);

function runGraphLookup(fromViewName, fromViewPipeline) {
    assert.commandWorked(db.createView(fromViewName, collName, fromViewPipeline));

    // Sanity check to make sure the view returns what we expect.
    const internationalAirportsView = db[fromViewName];
    assert.eq(internationalAirports, internationalAirportsView.count(), internationalAirportsView.find().toArray());

    const results = coll
        .aggregate([
            {$limit: 1}, // We only need one document to run the $graphLookup from.
            {
                $graphLookup: {
                    from: fromViewName,
                    startWith: "PWM",
                    connectFromField: "connects",
                    connectToField: "_id",
                    as: "connections",
                },
            },
        ])
        .toArray()[0];

    // It should retrieve all airports in the view.
    assert.eq(results.connections.length, internationalAirports, results);
}

// Test $graphLookup over a view that contains a $search stage.
runGraphLookup(collName + "_search_view", [
    {$search: {index: searchIndexSpec.name, text: {path: "name", query: "International"}}},
]);

// Test $graphLookup over a view that contains a $vectorSearch stage.
runGraphLookup(collName + "_vector_search_view", [
    {
        $vectorSearch: {
            queryVector: getAirportNameEmbeddingById("ORD"),
            index: vectorSearchIndexSpec.name,
            limit: internationalAirports,
            numCandidates: airports.length * 2,
            path: "nameEmbedding",
        },
    },
]);

// Test $graphLookup over a view that contains a subpipeline over a different view with a $search stage.
const nestedViewName = collName + "_nested_search_view_inner";
assert.commandWorked(
    db.createView(nestedViewName, collName, [
        {$search: {index: searchIndexSpec.name, text: {path: "name", query: "International"}}},
    ]),
);
runGraphLookup(collName + "_nested_search_view", [
    // Skip the original airports so that we only return the results of the unionWith.
    {$skip: airports.length},
    {$unionWith: {coll: nestedViewName, pipeline: []}},
]);

dropSearchIndex(coll, {name: searchIndexSpec.name});
dropSearchIndex(coll, {name: vectorSearchIndexSpec.name});
