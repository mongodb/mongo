/**
 * Test that an aggregation with a $out stage obeys the maxTimeMS.
 * @tags: [
 *   requires_replication,
 *   requires_sharding,
 * ]
 */
(function() {
load("jstests/libs/curop_helpers.js");    // For waitForCurOpByFailPoint().
load("jstests/libs/fixture_helpers.js");  // For isMongos().
load("jstests/libs/profiler.js");         // For profilerHasSingleMatchingEntryOrThrow.

const kDBName = "test";
const kSourceCollName = "out_max_time_ms_source";
const kDestCollName = "out_max_time_ms_dest";
// Picked `1000` documents to force the `$out` stage to write several batches.
const nDocs = 1000;

/**
 * Helper for populating the collection.
 */
function insertDocs(coll) {
    for (let i = 0; i < nDocs; i++) {
        assert.commandWorked(coll.insert({_id: i}, {writeConcern: {w: "majority"}}));
    }
}

// A list of connections to all the nodes currently being used by the test.
let connsToAllNodes = [];

/**
 * Prevents premature maxTimeMS expiration by enabling the "maxTimeNeverTimeOut" failpoint on each
 * node under test.
 */
function prohibitMaxTimeExpiration() {
    for (const conn of connsToAllNodes) {
        assert.commandWorked(conn.getDB("admin").runCommand(
            {configureFailPoint: "maxTimeNeverTimeOut", mode: "alwaysOn"}));
    }
}

/**
 * Given a mongod connection, run a $out aggregation against 'conn'. Set the provided failpoint on
 * the node specified by 'failPointConn' in order to hang during the aggregate. Ensure that the $out
 * maxTimeMS expires on the node specified by 'maxTimeMsConn'.
 */
function forceAggregationToHangAndCheckMaxTimeMsExpires(
    failPointName, conn, failPointConn, maxTimeMsConn) {
    // Use a short maxTimeMS so that the test completes in a reasonable amount of time. We will
    // use the 'maxTimeNeverTimeOut' failpoint to ensure that the operation does not prematurely
    // time out.
    const maxTimeMS = 1000 * 2;

    // Enable a failPoint so that the write will hang.
    const failpointCommand = {
        configureFailPoint: failPointName,
        mode: "alwaysOn",
    };

    assert.commandWorked(failPointConn.getDB("admin").runCommand(failpointCommand));

    // Make sure we don't run out of time on any of the involved nodes before the failpoint is hit.
    prohibitMaxTimeExpiration();

    // Build the parallel shell function.
    let shellStr = `const testDB = db.getSiblingDB('${kDBName}');`;
    shellStr += `const sourceColl = testDB['${kSourceCollName}'];`;
    shellStr += `const destColl = testDB['${kDestCollName}'];`;
    shellStr += `const maxTimeMS = ${maxTimeMS};`;
    const runAggregate = function() {
        const pipeline = [{$out: destColl.getName()}];
        const err = assert.throws(
            () => sourceColl.aggregate(
                pipeline, {maxTimeMS: maxTimeMS, $readPreference: {mode: "secondary"}}));
        assert.eq(err.code, ErrorCodes.MaxTimeMSExpired, "expected aggregation to fail");
    };
    shellStr += `(${runAggregate.toString()})();`;
    const awaitShell = startParallelShell(shellStr, conn.port);

    waitForCurOpByFailPointNoNS(failPointConn.getDB("admin"), failPointName);

    assert.commandWorked(maxTimeMsConn.getDB("admin").runCommand(
        {configureFailPoint: "maxTimeNeverTimeOut", mode: "off"}));

    // The aggregation running in the parallel shell will hang on the failpoint, burning
    // its time. Wait until the maxTimeMS has definitely expired.
    sleep(maxTimeMS + 2000);

    // Now drop the failpoint, allowing the aggregation to proceed. It should hit an
    // interrupt check and terminate immediately.
    assert.commandWorked(
        failPointConn.getDB("admin").runCommand({configureFailPoint: failPointName, mode: "off"}));

    // Wait for the parallel shell to finish.
    assert.eq(awaitShell(), 0);
}

/**
 * Run a $out aggregate against the node specified by 'conn' with primary 'primaryConn' (these may
 * be the same node). Verify that maxTimeMS properly times out the aggregate on the node specified
 * by 'maxTimeMsConn' both while hanging on the insert/update on 'primaryConn' and while hanging on
 * the batch being built on 'conn'.
 */
function runUnshardedTest(conn, primaryConn, maxTimeMsConn) {
    jsTestLog("Running unsharded test");

    const sourceColl = conn.getDB(kDBName)[kSourceCollName];
    const destColl = primaryConn.getDB(kDBName)[kDestCollName];
    assert.commandWorked(destColl.remove({}));

    // Be sure we're able to read from a cursor with a maxTimeMS set on it.
    (function() {
        // Use a long maxTimeMS, since we expect the operation to finish.
        const maxTimeMS = 1000 * 600;
        const pipeline = [{$out: destColl.getName()}];
        const cursor = sourceColl.aggregate(pipeline, {maxTimeMS: maxTimeMS});
        assert(!cursor.hasNext());
        assert.eq(destColl.countDocuments({_id: {$exists: true}}), nDocs);
    })();

    assert.commandWorked(destColl.remove({}));

    // Force the aggregation to hang while the batch is being written.
    const kFailPointName = "hangDuringBatchInsert";
    forceAggregationToHangAndCheckMaxTimeMsExpires(
        kFailPointName, conn, primaryConn, maxTimeMsConn);

    assert.commandWorked(destColl.remove({}));

    // Force the aggregation to hang while the batch is being built.
    forceAggregationToHangAndCheckMaxTimeMsExpires(
        "hangWhileBuildingDocumentSourceOutBatch", conn, conn, conn);
}

// Run on a standalone.
(function() {
const conn = MongoRunner.runMongod({});
assert.neq(null, conn, 'mongod was unable to start up');
connsToAllNodes = [conn];
insertDocs(conn.getDB(kDBName)[kSourceCollName]);
runUnshardedTest(conn, conn, conn);
MongoRunner.stopMongod(conn);
})();

// Run on the primary and the secondary of a replica set.
(function() {
const replTest = new ReplSetTest({nodes: 2});
replTest.startSet();
replTest.initiate();
replTest.awaitReplication();
const primary = replTest.getPrimary();
const secondary = replTest.getSecondary();
connsToAllNodes = [primary, secondary];
insertDocs(primary.getDB(kDBName)[kSourceCollName]);
// Run the $out on the primary and test that the maxTimeMS times out on the primary.
runUnshardedTest(primary, primary, primary);
// Run the $out on the secondary and test that the maxTimeMS times out on the primary.
runUnshardedTest(secondary, primary, primary);
// Run the $out on the secondary and test that the maxTimeMS times out on the secondary.
runUnshardedTest(secondary, primary, secondary);
replTest.stopSet();
})();
})();
