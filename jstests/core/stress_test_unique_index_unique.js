// Tests that unique indexes can be built with a large number of unique values

// @tags: [
//   assumes_unsharded_collection,
//   requires_non_retryable_writes,
// ]

(function() {
"use strict";

load("jstests/concurrency/fsm_workload_helpers/server_types.js");
// This workload would timeout on ephemeralForTest. isEphemeralForTest throws an exception if called
// from a mongos, so we need to check that first. Unique indexes are not supported on sharded
// collections anyway so it's safe to quit early if the db is a mongos.
if (isMongos(db) || isEphemeralForTest(db)) {
    return;
}

let coll = db.stress_test_unique_index_unique;
coll.drop();

const kNumDocs = 500000;  // ~15 MB

function loadCollectionWithDocs(collection, numDocs) {
    const kMaxChunkSize = 100000;

    let inserted = 0;
    while (inserted < numDocs) {
        let docs = [];
        for (let i = 0; i < kMaxChunkSize && inserted + docs.length < numDocs; i++) {
            docs.push({"a": inserted + i});
        }
        collection.insertMany(docs);
        inserted += docs.length;
    }
}

loadCollectionWithDocs(coll, kNumDocs);

assert.commandWorked(coll.createIndex({a: 1}, {unique: true}));
})();
