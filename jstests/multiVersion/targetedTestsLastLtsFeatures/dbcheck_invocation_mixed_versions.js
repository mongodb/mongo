/**
 * Test the dbCheck command invocation on mixed versions.
 *
 * TODO SERVER-78399: Remove this test once feature flag is removed.
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {
    assertForDbCheckErrorsForAllNodes,
    clearHealthLog,
    forEachNonArbiterNode,
    runDbCheck
} from "jstests/replsets/libs/dbcheck_utils.js";

// This test injects inconsistencies between replica set members; do not fail because of expected
// dbHash differences.
TestData.skipCheckDBHashes = true;

const dbName = "dbcheck_invocation_mixed_versions";
const collName = "dbcheck_invocation_mixed_versions";

function testDbCheckInvocationParameters(replSet) {
    // Clean up for the test.
    clearHealthLog(replSet);

    const primary = replSet.getPrimary();
    const db = primary.getDB(dbName);
    db[collName].insertMany([...Array(10000).keys()].map(x => ({_id: x})), {ordered: false});

    function checkEntryBounds(start, end) {
        forEachNonArbiterNode(replSet, function(node) {
            const healthlog = node.getDB("local").system.healthlog;
            const keyBoundsResult = healthlog.aggregate([
                {$match: {operation: "dbCheckBatch"}},
                {
                    $group: {
                        _id: null,
                        batchStart: {$min: "$data.batchStart._id"},
                        batchEnd: {$max: "$data.batchEnd._id"}
                    }
                }
            ]);

            assert(keyBoundsResult.hasNext(), "dbCheck put no batches in health log");

            const bounds = keyBoundsResult.next();
            assert.eq(bounds.batchStart, start, "dbCheck should start at correct key");
            assert.eq(bounds.batchEnd, end, "dbCheck should end at correct key");
        });
    }

    // Run a dbCheck on just a subset of the documents
    const start = 1000;
    const end = 9000;

    let dbCheckParameters = {minKey: start, maxKey: end};
    if (FeatureFlagUtil.isPresentAndEnabled(
            primary,
            "SecondaryIndexChecksInDbCheck",
            )) {
        dbCheckParameters = {start: {_id: start}, end: {_id: end}};
    }
    runDbCheck(replSet, db, collName, dbCheckParameters, true /*awaitCompletion*/);

    checkEntryBounds(start, end);
    assertForDbCheckErrorsForAllNodes(
        replSet, true /*assertForErrors*/, true /*assertForWarnings*/);

    // Now, clear the health logs again,
    clearHealthLog(replSet);
}

jsTestLog("Running test with latest primary, last-lts secondary");
const latestLastLTSReplSet = new ReplSetTest({
    name: "dbCheckSet",
    nodes: [{binVersion: "latest"}, {binVersion: "last-lts", rsConfig: {priority: 0}}],
    nodeOptions: {
        setParameter: {dbCheckHealthLogEveryNBatches: 1},
    }
});

latestLastLTSReplSet.startSet();
latestLastLTSReplSet.initiate();
latestLastLTSReplSet.awaitSecondaryNodes();

testDbCheckInvocationParameters(latestLastLTSReplSet);

latestLastLTSReplSet.stopSet();

jsTestLog("Running test with last-lts primary, latest secondary");
const lastLTSLatestReplSet = new ReplSetTest({
    name: "dbCheckSet",
    nodes: [{binVersion: "last-lts"}, {binVersion: "latest", rsConfig: {priority: 0}}],
    nodeOptions: {
        setParameter: {dbCheckHealthLogEveryNBatches: 1},
    }
});

lastLTSLatestReplSet.startSet();
lastLTSLatestReplSet.initiate();
lastLTSLatestReplSet.awaitSecondaryNodes();

testDbCheckInvocationParameters(lastLTSLatestReplSet);

lastLTSLatestReplSet.stopSet();
