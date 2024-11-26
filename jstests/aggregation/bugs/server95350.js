/**
 * SERVER-95350: Fix jstests/aggregation/api_version_stage_allowance_checks.js.
 */

(function() {
"use strict";

load("jstests/libs/collection_drop_recreate.js");  // For assertDropAndRecreateCollection.

const collName = 'test';
assertDropAndRecreateCollection(db, collName);

let docs = [];
for (let i = 0; i < 10; ++i) {
    docs.push({x: i, y: i, z: i});
}
db[collName].insertMany(docs);

assert.commandWorked(db.runCommand({
    explain: {
        aggregate: collName,
        pipeline: [{
            $mergeCursors: {
                sort: {y: 1, z: 1},
                compareWholeSortKey: false,
                remotes: [],
                nss: "test.mergeCursors",
                allowPartialResults: false,
            }
        }],
        cursor: {},
        readConcern: {},
    },
}));
}());
