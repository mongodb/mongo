/**
 * Verify that $sample push down works properly in a transaction. This test was designed to
 * reproduce SERVER-57642.
 *
 * Requires random cursor support.
 * @tags: [requires_replication]
 */
import {aggPlanHasStage} from "jstests/libs/query/analyze_plan.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

// Set up.
const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();
const collName = "sample_pushdown";
const dbName = "test";
const testDB = rst.getPrimary().getDB(dbName);
const coll = testDB[collName];

// In order to construct a plan that uses a storage engine random cursor, we not only need more
// than 100 records in our collection, we also need the sample size to be less than 5% of the
// number of documents in our collection.
const numDocs = 1000;
const sampleSize = numDocs * 0.03;
let docs = [];
for (let i = 0; i < numDocs; ++i) {
    docs.push({a: i});
}
assert.commandWorked(coll.insert(docs));
const pipeline = [{$sample: {size: sampleSize}}, {$match: {a: {$gte: 0}}}];

// Verify that our pipeline uses $sample push down.
const explain = coll.explain().aggregate(pipeline);
assert(aggPlanHasStage(explain, "$sampleFromRandomCursor"), tojson(explain));

// Start the transaction.
const session = testDB.getMongo().startSession({causalConsistency: false});
const sessionDB = session.getDatabase(dbName);
session.startTransaction();

// Run the pipeline.
const randDocs = sessionDB[collName].aggregate(pipeline).toArray();

// Verify that we have at least one result.
assert.gt(randDocs.length, 0, tojson(randDocs));

// Clean up.
assert.commandWorked(session.abortTransaction_forTesting());
rst.stopSet();
