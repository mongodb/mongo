/*
 * Tests that unique indexes can be built with a large number of non-unique values.
 */

(function() {
"use strict";

let coll = db.stress_test_unique_index_notunique;
coll.drop();

const kNumDocs = 500000;  // ~15 MB

function loadCollectionWithDocs(collection, numDocs) {
    const kMaxChunkSize = 100000;

    let inserted = 0;
    while (inserted < numDocs) {
        let docs = [];
        for (let i = 0; i < kMaxChunkSize && inserted + docs.length < numDocs; i++) {
            docs.push({"a": 0});
        }
        assert.commandWorked(collection.insertMany(docs, {ordered: false}));
        inserted += docs.length;
    }
}

loadCollectionWithDocs(coll, kNumDocs);

assert.commandFailedWithCode(coll.createIndex({a: 1}, {unique: true}), ErrorCodes.DuplicateKey);
})();
