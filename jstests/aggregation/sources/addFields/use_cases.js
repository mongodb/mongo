/**
 * $addFields can be used to add fixed and computed fields to documents while preserving the
 * original document. Verify that using $addFields and adding computed fields in a $project yield
 * the same result.
 */

(function() {
    "use strict";

    // For arrayEq.
    load("jstests/aggregation/extras/utils.js");

    const dbName = "test";
    const collName = jsTest.name();

    function doExecutionTest(conn) {
        const coll = conn.getDB(dbName).getCollection(collName);
        coll.drop();

        // Insert a bunch of documents of the form above.
        const nDocs = 10;
        for (let i = 0; i < nDocs; i++) {
            assert.writeOK(coll.insert({"_id": i, "2i": i * 2, "3i": i * 3}));
        }

        // Add the minimum, maximum, and average temperatures, and make sure that doing the same
        // with addFields yields the correct answer.
        // First compute with $project, since we know all the fields in this document.
        let projectPipe = [{
            $project: {
                "2i": 1,
                "3i": 1,
                "6i^2": {"$multiply": ["$2i", "$3i"]},
                // _id is implicitly included.
            }
        }];
        let correct = coll.aggregate(projectPipe).toArray();

        // Then compute the same results using $addFields.
        let addFieldsPipe = [{
            $addFields: {
                "6i^2": {"$multiply": ["$2i", "$3i"]},
                // All other fields are implicitly included.
            }
        }];
        let addFieldsResult = coll.aggregate(addFieldsPipe).toArray();

        // Then assert they are the same.
        assert(arrayEq(addFieldsResult, correct),
               "$addFields does not work the same as a $project with computed and included fields");
    }

    // Test against the standalone started by resmoke.py.
    let conn = db.getMongo();
    doExecutionTest(conn);
    print("Success! Standalone execution use case test for $addFields passed.");

    // Test against a sharded cluster.
    let st = new ShardingTest({shards: 2});
    doExecutionTest(st.s0);
    st.stop();
    print("Success! Sharding use case test for $addFields passed.");

}());