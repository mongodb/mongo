/**
 * $replaceRoot can be used to extract parts of a document; here we test a simple address case.
 */

(function() {
    "use strict";

    // For arrayEq.
    load("jstests/aggregation/extras/utils.js");

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
     *   name: <string>,
     *   address: {number: <3-digit int>, street: <string>, city: <string>, zip: <5-digit int>}
     * }
     */
    function generateRandomDocument() {
        let names = ["Asya", "Charlie", "Dan", "Geert", "Kyle"];
        const minNumber = 1;
        const maxNumber = 999;
        let streets = ["3rd", "4th", "5th", "6th", "7th", "8th", "9th"];
        let cities = ["New York", "Palo Alto", "Sydney", "Dublin"];
        const minZip = 10000;
        const maxZip = 99999;

        return {
            names: randomChoice(names),
            address: {
                number: Random.randInt(maxNumber - minNumber + 1) + minNumber,
                street: randomChoice(streets),
                city: randomChoice(cities),
                zip: Random.randInt(maxZip - minZip + 1) + minZip,
            },
        };
    }

    function doExecutionTest(conn) {
        const coll = conn.getDB(dbName).getCollection(collName);
        coll.drop();

        // Insert a bunch of documents of the form above.
        const nDocs = 10;
        let bulk = coll.initializeUnorderedBulkOp();
        for (let i = 0; i < nDocs; i++) {
            bulk.insert(generateRandomDocument());
        }
        assert.writeOK(bulk.execute());

        // Extract the contents of the address field, and make sure that doing the same
        // with replaceRoot yields the correct answer.
        // First compute each separately, since we know all of the fields in the address,
        // to make sure we have the correct results.
        let addressPipe = [{
            $project: {
                "_id": 0,
                "number": "$address.number",
                "street": "$address.street",
                "city": "$address.city",
                "zip": "$address.zip"
            }
        }];
        let correctAddresses = coll.aggregate(addressPipe).toArray();

        // Then compute the same results using $replaceWith.
        let replaceWithResult = coll.aggregate([
                                        {$replaceRoot: {newRoot: "$address"}},
                                        {$sort: {city: 1, zip: 1, street: 1, number: 1}}
                                    ])
                                    .toArray();

        // Then assert they are the same.
        assert(
            arrayEq(replaceWithResult, correctAddresses),
            "$replaceRoot does not work the same as $project-ing the relevant fields to the top level");
    }

    // Test against the standalone started by resmoke.py.
    let conn = db.getMongo();
    doExecutionTest(conn);
    print("Success! Standalone execution test for $replaceRoot passed.");

    // Test against a sharded cluster.
    let st = new ShardingTest({shards: 2});
    doExecutionTest(st.s0);
    st.stop();
    print("Success! Sharding test for $replaceRoot passed.");

}());