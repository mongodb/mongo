// This test verifies that an intermediate $lookup stage can grow larger than 16MB,
// but no larger than internalLookupStageIntermediateDocumentMaxSizeBytes.
// @tags: [requires_sharding]

load("jstests/aggregation/extras/utils.js");  // For assertErrorCode.

(function() {
    "use strict";

    // Used by testPipeline to sort result documents. All _ids must be primitives.
    function compareId(a, b) {
        if (a._id < b._id) {
            return -1;
        }
        if (a._id > b._id) {
            return 1;
        }
        return 0;
    }

    // Helper for testing that pipeline returns correct set of results.
    function testPipeline(pipeline, expectedResult, collection) {
        assert.eq(collection.aggregate(pipeline).toArray().sort(compareId),
                  expectedResult.sort(compareId));
    }

    function runTest(coll, from) {
        const db = null;  // Using the db variable is banned in this function.

        //
        // Confirm aggregation will not fail if intermediate $lookup stage exceeds 16 MB.
        //
        assert.writeOK(coll.insert([
            {"_id": 3, "same": 1},
        ]));

        const bigString = new Array(1025).toString();
        const doc = {_id: new ObjectId(), x: bigString, same: 1};
        const docSize = Object.bsonsize(doc);

        // Number of documents in lookup to exceed maximum BSON document size.
        // Using 20 MB instead to be safe.
        let numDocs = Math.floor(20 * 1024 * 1024 / docSize);

        let bulk = from.initializeUnorderedBulkOp();
        for (let i = 0; i < numDocs; ++i) {
            bulk.insert({x: bigString, same: 1});
        }
        assert.writeOK(bulk.execute());

        let pipeline = [
            {$lookup: {from: "from", localField: "same", foreignField: "same", as: "arr20mb"}},
            {$project: {_id: 1}}
        ];

        let expectedResults = [{_id: 3}];

        testPipeline(pipeline, expectedResults, coll);

        //
        // Confirm aggregation will fail if intermediate $lookup stage exceeds
        // internalLookupStageIntermediateDocumentMaxSizeBytes, set to 30 MB.
        //

        // Number of documents to exceed maximum intermediate $lookup stage document size.
        // Using 35 MB total to be safe (20 MB from previous test + 15 MB).
        numDocs = Math.floor(15 * 1024 * 1024 / docSize);

        bulk = from.initializeUnorderedBulkOp();
        for (let i = 0; i < numDocs; ++i) {
            bulk.insert({x: bigString, same: 1});
        }
        assert.writeOK(bulk.execute());

        pipeline = [
            {$lookup: {from: "from", localField: "same", foreignField: "same", as: "arr35mb"}},
            {$project: {_id: 1}}
        ];

        assertErrorCode(coll, pipeline, 4568);
    }

    // Run tests on single node.
    const standalone = MongoRunner.runMongod();
    const db = standalone.getDB("test");

    assert.commandWorked(db.adminCommand(
        {setParameter: 1, internalLookupStageIntermediateDocumentMaxSizeBytes: 30 * 1024 * 1024}));

    runTest(db.lookUp, db.from);

    MongoRunner.stopMongod(standalone);

    // Run tests in a sharded environment.
    const sharded = new ShardingTest({
        mongos: 1,
        shards: 2,
        rs: {
            nodes: 1,
            setParameter:
                {internalLookupStageIntermediateDocumentMaxSizeBytes: 30 * 1024 * 1024}
        }
    });

    assert(sharded.adminCommand({enableSharding: "test"}));

    assert(sharded.adminCommand({shardCollection: "test.lookUp", key: {_id: 'hashed'}}));
    runTest(sharded.getDB('test').lookUp, sharded.getDB('test').from);

    sharded.stop();
}());
