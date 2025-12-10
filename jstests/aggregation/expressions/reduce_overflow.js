/**
 * Verify the server does not crash when $reduce creates a deeply nested intermediate document.
 */
const coll = db[jsTestName()];

let seenSuccess = false;
let seenOverflow = false;
let recursiveObject = {"a": "$$value.array"};
let depth = 0;
for (let recursiveObjectDepth = 10; recursiveObjectDepth < 150; recursiveObjectDepth *= 2) {
    while (depth < recursiveObjectDepth) {
        recursiveObject = {"a": recursiveObject};
        depth = depth + 1;
    }

    const pipeline = [
        {"$group": {"_id": null, "entries": {"$push": "$value"}}},
        {
            "$project": {
                "filtered": {
                    "$reduce": {
                        "input": "$entries",
                        "initialValue": {"array": []},
                        "in": {"array": [recursiveObject]}
                    }
                }
            }
        }
    ];

    for (let numDocs = 10; numDocs < 500; numDocs *= 2) {
        coll.drop();
        const bulk = coll.initializeUnorderedBulkOp();
        for (let i = 0; i < numDocs; i++) {
            bulk.insert({"value": 0});
        }
        assert.commandWorked(bulk.execute());
        try {
            coll.aggregate(pipeline);
            seenSuccess = true;
            assert(!seenOverflow);
        } catch (error) {
            assert(seenSuccess);
            assert(error.code === ErrorCodes.Overflow, error);
            jsTest.log("Pipeline exceeded max BSON depth", numDocs, recursiveObjectDepth);
            seenOverflow = true;
        }
    }
}
assert(seenOverflow, "expected test to trigger overflow case");
