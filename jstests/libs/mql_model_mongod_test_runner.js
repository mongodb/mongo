/**
 * Test runner responsible for parsing and executing a MQL MongoD model test json file.
 */
(function() {
    "use strict";

    const jsonFilename = jsTestOptions().mqlTestFile;
    const mqlRootPath = jsTestOptions().mqlRootPath;

    if (jsonFilename === undefined) {
        throw new Error('MQL MongoD model tests must be run through resmoke.py');
    }

    // Populate collections with data fetched from the dataFile.
    function populateCollections(dataFile) {
        const data = JSON.parse(cat(mqlRootPath + dataFile));

        data.forEach(function(singleColl) {
            assert(singleColl.hasOwnProperty("namespace"), "MQL data model requires a 'namespace'");
            assert(singleColl.hasOwnProperty("data"), "MQL data model requires a 'data'");

            const coll = db.getCollection(singleColl["namespace"]);
            coll.drop();

            singleColl["data"].forEach(function(doc) {
                assert.commandWorked(coll.insert(doc));
            });
        });
    }

    // Run a single find test.
    function runFindTest(testFile, dataFile, expected) {
        populateCollections(dataFile);

        const test = JSON.parse(cat(mqlRootPath + testFile));

        const results = db.getCollection(test["find"]).find(test["filter"], {_id: 0}).toArray();

        assert.eq(results, expected);
    }

    // Read a list of tests from the jsonFilename and execute them.
    const testList = JSON.parse(cat(jsonFilename));
    testList.forEach(function(singleTest) {
        if (singleTest.hasOwnProperty("match")) {
            // Skip the match test type as it is not directly supported by mongod.
        } else if (singleTest.hasOwnProperty("find")) {
            // Run the find test type.
            assert(singleTest.hasOwnProperty("data"), "MQL model test requires a 'data'");
            assert(singleTest.hasOwnProperty("expected"), "MQL model test requires a 'expected'");

            runFindTest(singleTest["find"], singleTest["data"], singleTest["expected"]);
        } else {
            throw new Error("Unknown test type: " + tojson(singleTest));
        }
    });
}());
