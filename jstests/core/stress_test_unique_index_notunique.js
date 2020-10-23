// Tests that unique indexes can be built with a large number of non-unique values

// @tags: [
//   assumes_unsharded_collection,
//   requires_non_retryable_writes,
// ]

(function() {
"use strict";

let coll = db.stress_test_unique_index_notunique;
coll.drop();

const kNumDocs = 2000000;  // ~65 MB

function loadCollectionWithDocs(collection, numDocs) {
    const kMaxChunkSize = 100000;

    let inserted = 0;
    while (inserted < numDocs) {
        let docs = [];
        for (let i = 0; i < kMaxChunkSize && inserted + docs.length < numDocs; i++) {
            docs.push({"a": 0});
        }
        collection.insertMany(docs);
        inserted += docs.length;
    }
}

loadCollectionWithDocs(coll, kNumDocs);

assert.commandFailedWithCode(coll.createIndex({a: 1}, {unique: true}), ErrorCodes.DuplicateKey);
})();
