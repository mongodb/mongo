/**
 * Tests some practical use cases of the $facet stage.
 */
(function() {
"use strict";

load("jstests/noPassthrough/libs/server_parameter_helpers.js");  // For setParameterOnAllHosts.
load("jstests/libs/discover_topology.js");                       // For findDataBearingNodes.

const dbName = "test";
const collName = jsTest.name();

Random.setRandomSeed();

/**
 * Helper to get a random entry out of an array.
 */
function randomChoice(array) {
    return array[Random.randInt(array.length)];
}

/**
 * Helper to generate a randomized document with the following schema:
 * {
 *   manufacturer: <string>,
 *   price: <double>,
 *   screenSize: <double>
 * }
 */
function generateRandomDocument(docId) {
    const manufacturers =
        ["Sony", "Samsung", "LG", "Panasonic", "Mitsubishi", "Vizio", "Toshiba", "Sharp"];
    const minPrice = 100;
    const maxPrice = 4000;
    const minScreenSize = 18;
    const maxScreenSize = 40;

    return {
        _id: docId,
        manufacturer: randomChoice(manufacturers),
        price: Random.randInt(maxPrice - minPrice + 1) + minPrice,
        screenSize: Random.randInt(maxScreenSize - minScreenSize + 1) + minScreenSize,
    };
}

/**
 * Inserts 'nDocs' documents into collection given by 'dbName' and 'collName'. Documents will
 * have _ids in the range [0, nDocs).
 */
function populateData(nDocs) {
    var coll = db.getCollection(collName);
    coll.remove({});  // Don't drop the collection, since it might be sharded.

    var bulk = coll.initializeUnorderedBulkOp();
    for (var i = 0; i < nDocs; i++) {
        const doc = generateRandomDocument(i);
        bulk.insert(doc);
    }
    assert.commandWorked(bulk.execute());
}

const nDocs = 1000 * 10;
populateData(nDocs);
const coll = db.getCollection(collName);

//
// Compute the most common manufacturers, and the number of TVs in each price range.
//

// First compute each separately, to make sure we have the correct results.
const manufacturerPipe = [
    {$sortByCount: "$manufacturer"},
    // Sort by count and then by _id in case there are two manufacturers with an equal
    // count.
    {$sort: {count: -1, _id: 1}},
];
const bucketedPricePipe = [
    {
        $bucket: {groupBy: "$price", boundaries: [0, 500, 1000, 1500, 2000], default: 2000},
    },
    {$sort: {count: -1}}
];
const automaticallyBucketedPricePipe = [{$bucketAuto: {groupBy: "$price", buckets: 5}}];

const mostCommonManufacturers = coll.aggregate(manufacturerPipe).toArray();
const numTVsBucketedByPriceRange = coll.aggregate(bucketedPricePipe).toArray();
const numTVsAutomaticallyBucketedByPriceRange =
    coll.aggregate(automaticallyBucketedPricePipe).toArray();

const facetPipe = [{
    $facet: {
        manufacturers: manufacturerPipe,
        bucketedPrices: bucketedPricePipe,
        autoBucketedPrices: automaticallyBucketedPricePipe
    }
}];

// Then compute the results using $facet.
const facetResult = coll.aggregate(facetPipe).toArray();
assert.eq(facetResult.length, 1);
const facetManufacturers = facetResult[0].manufacturers;
const facetBucketedPrices = facetResult[0].bucketedPrices;
const facetAutoBucketedPrices = facetResult[0].autoBucketedPrices;

// Then assert they are the same.
assert.eq(facetManufacturers, mostCommonManufacturers);
assert.eq(facetBucketedPrices, numTVsBucketedByPriceRange);
assert.eq(facetAutoBucketedPrices, numTVsAutomaticallyBucketedByPriceRange);
}());
