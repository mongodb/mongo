/**
 * Tests some practical use cases of the $facet stage.
 * @tags: [
 *   requires_sharding,
 *   requires_spawning_own_processes,
 * ]
 */
(function() {
"use strict";

load("jstests/noPassthrough/libs/server_parameter_helpers.js");  // For setParameterOnAllHosts.
load("jstests/libs/discover_topology.js");                       // For findDataBearingNodes.

const dbName = "test";
const collName = jsTest.name();
const testNs = dbName + "." + collName;

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
function populateData(conn, nDocs) {
    var coll = conn.getDB(dbName).getCollection(collName);
    coll.remove({});  // Don't drop the collection, since it might be sharded.

    var bulk = coll.initializeUnorderedBulkOp();
    for (var i = 0; i < nDocs; i++) {
        const doc = generateRandomDocument(i);
        bulk.insert(doc);
    }
    assert.commandWorked(bulk.execute());
}

function doExecutionTest(conn) {
    var coll = conn.getDB(dbName).getCollection(collName);
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
}

// Test against the standalone started by resmoke.py.
const nDocs = 1000 * 10;
const conn = db.getMongo();
populateData(conn, nDocs);
doExecutionTest(conn);

// Test against a sharded cluster.
const st = new ShardingTest({shards: 2});
populateData(st.s0, nDocs);
doExecutionTest(st.s0);

const shardedDBName = "sharded";
const shardedCollName = "collection";
const shardedColl = st.getDB(shardedDBName).getCollection(shardedCollName);
const unshardedColl = st.getDB(shardedDBName).getCollection(collName);

assert.commandWorked(st.admin.runCommand({enableSharding: shardedDBName}));
assert.commandWorked(
    st.admin.runCommand({shardCollection: shardedColl.getFullName(), key: {_id: 1}}));

// Test $lookup inside a $facet stage on a sharded collection.
// Enable sharded $lookup.
setParameterOnAllHosts(
    DiscoverTopology.findNonConfigNodes(st.s), "internalQueryAllowShardedLookup", true);
assert.commandWorked(unshardedColl.runCommand({
    aggregate: unshardedColl.getName(),
    pipeline: [{
        $facet: {
            a: [{
                $lookup:
                    {from: shardedCollName, localField: "_id", foreignField: "_id", as: "results"}
            }]
        }
    }],
    cursor: {}
}));
// Disable sharded $lookup.
setParameterOnAllHosts(
    DiscoverTopology.findNonConfigNodes(st.s), "internalQueryAllowShardedLookup", false);

// Then run the assertions against a sharded collection.
assert.commandWorked(st.admin.runCommand({enableSharding: dbName}));
assert.commandWorked(st.admin.runCommand({shardCollection: testNs, key: {_id: 1}}));

// Make sure there is a chunk on each shard, so that our aggregations are targeted to multiple
// shards.
assert.commandWorked(st.admin.runCommand({split: testNs, middle: {_id: nDocs / 2}}));
assert.commandWorked(
    st.admin.runCommand({moveChunk: testNs, find: {_id: 0}, to: st.shard0.shardName}));
assert.commandWorked(
    st.admin.runCommand({moveChunk: testNs, find: {_id: nDocs - 1}, to: st.shard1.shardName}));

doExecutionTest(st.s0);

st.stop();
}());
