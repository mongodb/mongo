// Test that the bucket unpacking with sorting rewrite is performed during a granularity change.
// This can cause buckets to exceed the max time span which could cause correctness issues.
// We check that the results are correct, the documents are sorted, and the documents we expect to
// appear, appear.
// Note: events in buckets that exceed bucketMaxSpan are not included.
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";

const dbName = jsTestName();

// Start a single mongoD using MongoRunner.
const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up");

// Create the test DB and collection.
const db = conn.getDB(dbName);
const adminDB = conn.getDB("admin");
const collName = dbName;
const coll = db[collName];
const minsToMillis = (mins) => mins * 60 * 1000;

const on = "alwaysOn";
const off = "off";

function setAggHang(mode) {
    assert.commandWorked(adminDB.adminCommand(
        {configureFailPoint: "hangBeforeDocumentSourceCursorLoadBatch", mode: mode}));
}

// Setup scenario.
const timeField = "t";
coll.drop();
db.createCollection(collName, {timeseries: {timeField: timeField}});
assert.commandWorked(coll.insert({[timeField]: new Date(minsToMillis(0))}));
assert.commandWorked(coll.insert({[timeField]: new Date(minsToMillis(60) - 1)}));
assert.commandWorked(coll.insert({[timeField]: new Date(minsToMillis(60))}));

// Enable the hang point.
setAggHang(on);

// Start parallel shell that checks that newly inserted info is found in naive sorter.
const aggregateNaive = `
  	  const testDB = db.getSiblingDB('${dbName}');
	  const testColl = testDB['${dbName}'];
	  const results = testColl.aggregate([{$_internalInhibitOptimization: {}}, {$sort: {t: 1}}]).toArray();
	  assert.eq(results.length, 4, results);
	`;
// To assist in debugging we log the explain output.
const aggregateExpOptimized = `
	  const testDB = db.getSiblingDB('${dbName}');
      const testColl = testDB['${dbName}'];
      const results = testColl.explain().aggregate([{$sort: {t: -1}}], {hint: {$natural: -1}});
      jsTestLog(results);
	`;
// Start parallel shell that checks that newly inserted info is not found in bounded sorter.
const aggregateOptimized = `
  	  const testDB = db.getSiblingDB('${dbName}');
	  const testColl = testDB['${dbName}'];
	  const results = testColl.aggregate([{$sort: {t: -1}}], {hint: {$natural: -1}}).toArray();
	  assert.eq(results.length, 2, results);
	`;

// At this point in time we have inserted three documents, two going into one bucket A, and another
// into another bucket B. With Bucket Unpacking Sorting enabled, which happens in the
// aggregateOptimized command but not the aggregateNaive command, if a bucket that we are unpacking
// has had its granularity change while we are unpacking (which we induce in this test by hanging on
// a failpoint, changing the granularity, and then turning off the hang point), we do not return the
// measurements within it as part of our query results to avoid potentially returning results out of
// order. Without the feature flag enabled, after inserting our third measurement we would have
// caused bucket A to rollover due to timeForward, meaning that our fourth insertion, which happens
// after we change the granularity, could only go into bucket B. The check for granularity is done
// by taking the difference in time between the bucket's min and max time and checking whether this
// difference is greater than its bucketMaxSpan.
//
// However, with the feature flag enabled, both A and B will be open and eligible for the insert. If
// the measurement is inserted into B, as used to always be the case, the number of measurements
// returned and checked for in aggregateOptimized will be 2 - the total 4, minus the 2 in B, whose
// measurements would be omitted. If instead the measurement is inserted into bucket A, the total
// number returned will be 1 - the total 4, minus the 3 in bucket A. TODO: SERVER-79480 - Re-enable
// this test once we have determined criteria for selecting the best open bucket for a measurement,
// when there are multiple open ones, to make which bucket gets selected deterministic.
if (TimeseriesTest.timeseriesAlwaysUseCompressedBucketsEnabled(adminDB)) {
    // Turn off the hang and stop the mongod runner.
    setAggHang(off);
    MongoRunner.stopMongod(conn);
    quit();
}

const mergeShellNaive = startParallelShell(aggregateNaive, db.getMongo().port);
const mergeShellExplain = startParallelShell(aggregateExpOptimized, db.getMongo().port);
const mergeShellOptimized = startParallelShell(aggregateOptimized, db.getMongo().port);

// Wait for the parallel shells to hit the failpoint.
assert.soon(() => db.currentOp({
                        op: "command",
                        "command.aggregate": dbName,
                        "command.explain": {$exists: false}
                    }).inprog.length == 2,
            () => tojson(db.currentOp().inprog));

// Reconfigure collection parameters
assert.commandWorked(db.runCommand({collMod: dbName, timeseries: {granularity: "hours"}}));

// Insert data that will fit in the new wider bucket.
assert.commandWorked(coll.insert({[timeField]: new Date(minsToMillis(120) + 1)}));

// Turn off the hang.
setAggHang(off);

jsTestLog(db.system.buckets[dbName].find().toArray());

// Double check that the number of buckets is expected.
assert.eq(db.system.buckets[dbName].find().toArray().length, 2);

// Finish the computation.
let resNaive = mergeShellNaive();
assert(resNaive == 0);
let resExp = mergeShellExplain();
assert(resExp == 0);
let resOpt = mergeShellOptimized();
assert(resOpt == 0);

MongoRunner.stopMongod(conn);
