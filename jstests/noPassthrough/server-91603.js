/**
 * Tests that downgrading during an operation that checks featureFlagBinDataConvert twice results in
 * a successful operation.
 *
 * @tags: [
 *   requires_fcv_80,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";

const conn = MongoRunner.runMongod();
const db = conn.getDB("test");
db.coll.insert({_id: 1, a: 1});

// Enable a failpoint that will cause us to hang right before the second check of
// featureFlagBinDataConvert.
const failPoint = configureFailPoint(db, "hangBeforeSecondFeatureFlagBinDataConvertCheck");

// Run a query in a parallel shell that would end up needing the value of the FF twice.
const queryParallelShell = startParallelShell(
    () => db.coll.aggregate([{$project: {output: {$toString: 1.19}}}]).toArray(), conn.port);

// Wait until we hit the failpoint to continue.
failPoint.wait();

// While we are hanging, downgrade the FCV to 7.0 (a version where featureFlagBinDataConvert is not
// enabled).
assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: "7.0", confirm: true}));

// Disable the hang and wait for the query to complete.
failPoint.off();

// Assert that the parallel shell exited sucessfully to ensure that the query executed correctly
// (and that we don't fail due to reading different feature flag values).
queryParallelShell({checkExitSuccess: true});

MongoRunner.stopMongod(conn);
