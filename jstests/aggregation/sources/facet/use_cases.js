/**
 * Tests some practical use cases of the $facet stage.
 */
(function() {
    "use strict";
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
    function generateRandomDocument() {
        const manufacturers =
            ["Sony", "Samsung", "LG", "Panasonic", "Mitsubishi", "Vizio", "Toshiba", "Sharp"];
        const minPrice = 100;
        const maxPrice = 4000;
        const minScreenSize = 18;
        const maxScreenSize = 40;

        return {
            manufacturer: randomChoice(manufacturers),
            price: Random.randInt(maxPrice - minPrice + 1) + minPrice,
            screenSize: Random.randInt(maxScreenSize - minScreenSize + 1) + minScreenSize,
        };
    }

    function doExecutionTest(conn) {
        var coll = conn.getDB(dbName).getCollection(collName);
        coll.drop();

        const nDocs = 1000 * 10;
        var bulk = coll.initializeUnorderedBulkOp();
        for (var i = 0; i < nDocs; i++) {
            const doc = generateRandomDocument();
            bulk.insert(doc);
        }
        assert.writeOK(bulk.execute());

        //
        // Compute the most common manufacturers, and the number of TVs in each price range.
        //

        // First compute each separately, to make sure we have the correct results.
        const manufacturerPipe =
            [{$group: {_id: "$manufacturer", count: {$sum: 1}}}, {$sort: {count: -1}}];
        const mostCommonManufacturers = coll.aggregate(manufacturerPipe).toArray();

        const pricePipe = [
            {
              $project: {
                  priceBucket: {
                      $switch: {
                          branches: [
                              {case: {$lt: ["$price", 500]}, then: "< 500"},
                              {case: {$lt: ["$price", 1000]}, then: "500-1000"},
                              {case: {$lt: ["$price", 1500]}, then: "1000-1500"},
                              {case: {$lt: ["$price", 2000]}, then: "1500-2000"}
                          ],
                          default: "> 2000"
                      }
                  }
              }
            },
            {$group: {_id: "$priceBucket", count: {$sum: 1}}},
            {$sort: {count: -1}}
        ];
        const numTVsByPriceRange = coll.aggregate(pricePipe).toArray();

        // Then compute the results using $facet.
        const facetResult =
            coll.aggregate([{$facet: {manufacturers: manufacturerPipe, prices: pricePipe}}])
                .toArray();
        assert.eq(facetResult.length, 1);
        const facetManufacturers = facetResult[0].manufacturers;
        const facetPrices = facetResult[0].prices;

        // Then assert they are the same.
        assert.eq(facetManufacturers, mostCommonManufacturers);
        assert.eq(facetPrices, numTVsByPriceRange);
    }

    // Test against the standalone started by resmoke.py.
    const conn = db.getMongo();
    doExecutionTest(conn);

    // Test against a sharded cluster.
    const st = new ShardingTest({shards: 2});
    doExecutionTest(st.s0);

    // Test that $facet stage propagates information about involved collections, preventing users
    // from doing things like $lookup from a sharded collection.
    const shardedDBName = "sharded";
    const shardedCollName = "collection";
    const shardedColl = st.getDB(shardedDBName).getCollection(shardedCollName);
    const unshardedColl = st.getDB(shardedDBName).getCollection(collName);

    assert.commandWorked(st.admin.runCommand({enableSharding: shardedDBName}));
    assert.commandWorked(
        st.admin.runCommand({shardCollection: shardedColl.getFullName(), key: {_id: 1}}));
    assert.commandFailed(unshardedColl.runCommand({
        aggregate: unshardedColl,
        pipline: [{
            $lookup:
                {from: shardedCollName, localField: "_id", foreignField: "_id", as: "results"}
        }]
    }));
    assert.commandFailed(unshardedColl.runCommand({
        aggregate: unshardedColl,
        pipeline: [{
            $facet: {
                a: [{
                    $lookup: {
                        from: shardedCollName,
                        localField: "_id",
                        foreignField: "_id",
                        as: "results"
                    }
                }]
            }
        }]
    }));
    st.stop();
}());
