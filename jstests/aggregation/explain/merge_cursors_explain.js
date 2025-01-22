/**
 * SERVER-95350: Running explain in some aggregation commands could trigger an invariant failure.
 */

import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";

const collName = 'test';
assertDropAndRecreateCollection(db, collName);

let docs = [];
for (let i = 0; i < 10; ++i) {
    docs.push({x: i, y: i, z: i});
}
db[collName].insertMany(docs);

// This command triggered the invariant failure. We expect it to pass with the fix made.
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
