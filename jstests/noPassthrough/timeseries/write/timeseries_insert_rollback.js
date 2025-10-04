/**
 * Tests that time-series inserts behave properly after previous time-series inserts were rolled
 * back.
 *
 * @tags: [
 *     requires_replication,
 *     requires_mongobridge,
 * ]
 */
import {getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";
import {RollbackTest} from "jstests/replsets/libs/rollback_test.js";

const rollbackTest = new RollbackTest(jsTestName());

const primary = rollbackTest.getPrimary();
const testDB = primary.getDB("test");
const coll = testDB[jsTestName()];

const timeFieldName = "time";
const metaFieldName = "m";

assert.commandWorked(
    testDB.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}),
);

rollbackTest.transitionToRollbackOperations();

const docs = [
    {_id: 0, [timeFieldName]: ISODate(), [metaFieldName]: "ordered"},
    {_id: 1, [timeFieldName]: ISODate(), [metaFieldName]: "unordered"},
    {_id: 2, [timeFieldName]: ISODate(), [metaFieldName]: "ordered"},
    {_id: 3, [timeFieldName]: ISODate(), [metaFieldName]: "unordered"},
];

// Insert new buckets that will be rolled back.
assert.commandWorked(coll.insert(docs[0], {ordered: true}));
assert.commandWorked(coll.insert(docs[1], {ordered: false}));

// Perform the rollback.
rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
rollbackTest.transitionToSyncSourceOperationsDuringRollback();
rollbackTest.transitionToSteadyStateOperations();

// Cycle through the rollback test phases so that the original primary becomes primary again.
rollbackTest.transitionToRollbackOperations();
rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
rollbackTest.transitionToSyncSourceOperationsDuringRollback();
rollbackTest.transitionToSteadyStateOperations();

// The in-memory bucket catalog should have been cleared by the rollback, so inserts will not
// attempt to go into now-nonexistent buckets.
assert.commandWorked(coll.insert(docs[2], {ordered: true}));
assert.commandWorked(coll.insert(docs[3], {ordered: false}));

assert.sameMembers(docs.slice(2), coll.find().toArray());
const buckets = getTimeseriesCollForRawOps(testDB, coll).find().rawData().toArray();
assert.eq(buckets.length, 2, "Expected two bucket but found: " + tojson(buckets));

rollbackTest.stop();
