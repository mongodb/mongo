/**
 * Inserts documents with an indexed nested array field, progressively increasing the nesting
 * depth until the key is too large to index. This tests that we support at least the minimum
 * supported BSON nesting depth, as well as maintaining index consistency.
 */
(function() {
    "use strict";

    function makeNestArr(depth) {
        if (depth == 1) {
            return {a: 1};
        } else if (depth == 2) {
            return {a: [1]};
        } else {
            return {a: [makeNestArr(depth - 2)]};
        }
    }

    let collection = db.arrNestTest;
    collection.drop();

    assert.commandWorked(collection.ensureIndex({a: 1}));

    const kMaxDocumentDepthSoftLimit = 100;
    const kJavaScriptMaxDepthLimit = 150;

    let level;
    for (level = 1; level < kJavaScriptMaxDepthLimit - 3; level++) {
        let res = db.runCommand({insert: collection.getName(), documents: [makeNestArr(level)]});
        if (!res.ok) {
            assert.commandFailedWithCode(
                res, 17280, "Expected insertion to fail only because key is too large to index");
            break;
        }
    }

    assert.gt(level,
              kMaxDocumentDepthSoftLimit,
              "Unable to insert a document nested with " + level +
                  " levels, which is less than the supported limit of " +
                  kMaxDocumentDepthSoftLimit);
    assert.eq(collection.count(),
              collection.find().hint({a: 1}).itcount(),
              "Number of documents in collection does not match number of entries in index");
}());
