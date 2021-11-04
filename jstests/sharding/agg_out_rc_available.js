/**
 * Tests that executing aggregate with $out with "available" read concern on sharded clusters
 * doesn't fail.
 */
(function() {
"use strict";

load('jstests/aggregation/extras/utils.js');

const st = new ShardingTest({shards: {rs0: {nodes: 1}}});
const dbName = "test";
db = st.getDB(dbName);

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));

// Setup and populate input collection.
const inputCollName = "input_coll";
const inputColl = db[inputCollName];

const inputDocs = [{_id: 1, x: 11}, {_id: 2, x: 22}, {_id: 3, x: 33}];
assert.commandWorked(inputColl.insert(inputDocs));

// Run a simple agg pipeline with $out and a readConcern of 'available' and assert that the command
// doesn't fail.
const outputCollName = "output_coll";
assert.commandWorked(db.runCommand({
    aggregate: inputCollName,
    pipeline: [{$out: outputCollName}],
    cursor: {},
    readConcern: {level: "available"}
}));

// Verify that the output collection contains the docments from the input collection.
const result = assert.commandWorked(db.runCommand({
    aggregate: outputCollName,
    pipeline: [{$match: {}}],
    cursor: {},
    readConcern: {level: "available"}
}));
assert(resultsEq(result.cursor.firstBatch, inputDocs), result.cursor);

st.stop();
})();
