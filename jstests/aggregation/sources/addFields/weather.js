/**
 * $addFields can be used to add fixed and computed fields to documents while preserving the
 * original document. Verify that using $addFields and adding computed fields in a $project yield
 * the same result. Use the sample case of computing weather metadata.
 */

(function() {
    "use strict";

    // For arrayEq.
    load("jstests/aggregation/extras/utils.js");

    const dbName = "test";
    const collName = jsTest.name();

    Random.setRandomSeed();

    /**
     * Helper to generate an array of specified length of numbers in the specified range.
     */
    function randomArray(length, minValue, maxValue) {
        let array = [];
        for (let i = 0; i < length; i++) {
            array.push((Random.rand() * (maxValue - minValue)) + minValue);
        }
        return array;
    }

    /**
     * Helper to generate a randomized document with the following schema:
     * {
     *    month: <integer month of year>,
     *    day: <integer day of month>,
     *    temperatures: <array of 24 decimal temperatures>
     * }
     */
    function generateRandomDocument() {
        const minTemp = -40;
        const maxTemp = 120;

        return {
            month: Random.randInt(12) + 1,  // 1-12
            day: Random.randInt(31) + 1,    // 1-31
            temperatures: randomArray(24, minTemp, maxTemp),
        };
    }

    function doExecutionTest(conn) {
        const coll = conn.getDB(dbName).getCollection(collName);
        coll.drop();

        // Insert a bunch of documents of the form above.
        const nDocs = 10;
        for (let i = 0; i < nDocs; i++) {
            assert.writeOK(coll.insert(generateRandomDocument()));
        }

        // Add the minimum, maximum, and average temperatures, and make sure that doing the same
        // with addFields yields the correct answer.
        // First compute with $project, since we know all the fields in this document.
        let projectWeatherPipe = [{
            $project: {
                "month": 1,
                "day": 1,
                "temperatures": 1,
                "minTemp": {"$min": "$temperatures"},
                "maxTemp": {"$max": "$temperatures"},
                "average": {"$avg": "$temperatures"},
                // _id is implicitly included.
            }
        }];
        let correctWeather = coll.aggregate(projectWeatherPipe).toArray();

        // Then compute the same results using $addFields.
        let addFieldsWeatherPipe = [{
            $addFields: {
                "minTemp": {"$min": "$temperatures"},
                "maxTemp": {"$max": "$temperatures"},
                "average": {"$avg": "$temperatures"},
                // All other fields are implicitly included.
            }
        }];
        let addFieldsResult = coll.aggregate(addFieldsWeatherPipe).toArray();

        // Then assert they are the same.
        assert(arrayEq(addFieldsResult, correctWeather),
               "$addFields does not work the same as a $project with computed and included fields");
    }

    // Test against the standalone started by resmoke.py.
    let conn = db.getMongo();
    doExecutionTest(conn);
    print("Success! Standalone execution weather test for $addFields passed.");

    // Test against a sharded cluster.
    let st = new ShardingTest({shards: 2});
    doExecutionTest(st.s0);
    st.stop();
    print("Success! Sharding weather test for $addFields passed.");

}());