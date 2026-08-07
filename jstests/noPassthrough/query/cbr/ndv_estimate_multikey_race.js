/**
 * Simulates a race condition between CBR sampling and a concurrent write that transitions an
 * index from non-multikey to multikey.
 *
 * The race:
 *  1. QueryPlanner::plan() reads the index as non-multikey; IndexScanNode::index.multikey = false.
 *  2. generateSample() starts a sequential collection scan.
 *  3. The scan yields: abandonSnapshot() releases the storage snapshot.
 *  4. A concurrent write inserts {a: [1, 2, 3]}, making the compound index multikey on path 'a'.
 *  5. The scan resumes on a newer snapshot and includes the array document in _sample.
 * The result is an inconsistency between what the IndexScanNode claims (not multikey) vs.
 * the document in the sample (which does contain an array).
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";

const conn = MongoRunner.runMongod({
    setParameter: {
        internalQueryFrameworkControl: "forceClassicEngine",
        featureFlagCostBasedRanker: true,
        internalQueryPlanRanker: "costBased",
        internalQueryCBRCEMode: "samplingCE",
        // Sequential scan gives a deterministic, repeatable sample order and makes it easy
        // to ensure the yield window is wide enough to include the new array document.
        internalQuerySamplingBySequentialScan: true,
        // Yield on every document so the scan reliably yields inside the sampling executor.
        internalQueryExecYieldIterations: 1,
        internalQueryExecYieldPeriodMS: 0,
    },
});
assert.neq(null, conn, "mongod failed to start");

const adminDB = conn.getDB("admin");
const testDB = conn.getDB(jsTestName());
const coll = testDB[jsTestName()];
coll.drop();

// Insert a document with scalar values so the index is initially non-multikey.
assert.commandWorked(coll.insertOne({a: 0, b: 0, c: 0}));
// Having this index but only restricting the `a` and `c` fields results in a skip scan.
assert.commandWorked(coll.createIndexes([{a: 1, b: 1, c: 1}]));

// Step 1: Pause just before generateSample() so that we can arm setYieldAllLocksHang after
// QueryPlanner::plan() has already built the IndexScanNode (node->index.multikey = false).
// explain() is used so CBR ranks even a single-solution query.
const fpBeforeSampling = configureFailPoint(adminDB, "hangBeforeCBRSamplingGenerateSample");

const awaitQuery = startParallelShell(() => {
    const killedMsg = 'query plan killed :: non-array path became multikey during yield: path=a"';
    const testColl = db.getSiblingDB(jsTestName())[jsTestName()];
    const err = assert.throws(() => testColl.find({a: {$lt: 50}, c: {$lt: 50}}).explain());
    assert.eq(err.code, ErrorCodes.QueryPlanKilled, err.message);
    assert(err.message.includes(killedMsg), err.message);
}, conn.port);

// Step 2: Wait for planning to complete (multikey = false in IndexScanNode).
fpBeforeSampling.wait();

// Step 3: Arm the yield failpoint so we catch the first yield inside the sampling scan.
// After this yield, abandonSnapshot() will have released the storage snapshot.
const fpYield = configureFailPoint(adminDB, "setYieldAllLocksHang", {
    namespace: coll.getFullName(),
});
fpBeforeSampling.off();

// Step 4: Wait for the sampling scan to yield.
fpYield.wait();

// Step 5: Insert an array-valued document, making the compound index multikey on path 'a'.
// The insert commits before the scan resumes, so the new snapshot will include this document.
assert.commandWorked(coll.insertOne({a: [1, 2, 3], b: 1}));

// Step 6: Release the yield. The scan resumes on a new snapshot that includes {a:[1,2,3]}.
fpYield.off();

awaitQuery();

MongoRunner.stopMongod(conn);
