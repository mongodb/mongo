/**
 * Test that dbCheck works in a multiversion replica set.
 * Primary should not set nss and uuid and secondary should be able to parse that health log.
 *
 * TODO SERVER-78399: Remove this test once feature flag is removed.
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {
    assertForDbCheckErrorsForAllNodes,
    checkHealthLog,
    clearHealthLog,
    runDbCheck
} from "jstests/replsets/libs/dbcheck_utils.js";

// This test injects inconsistencies between replica set members; do not fail because of expected
// dbHash differences.
TestData.skipCheckDBHashes = true;

const dbName = "dbcheck_start_stop_mixed_versions";
const collName = "dbcheck_start_stop_mixed_versions";

function testDbCheckStartStopEntries(replSet) {
    // Clean up for the test.
    clearHealthLog(replSet);

    const primary = replSet.getPrimary();
    const secondary = replSet.getSecondary();
    const db = primary.getDB(dbName);
    db[collName].insertMany([...Array(10000).keys()].map(x => ({_id: x})), {ordered: false});

    runDbCheck(replSet, db, collName, {} /*parameters*/, true /*awaitCompletion*/);

    // primary should not set nss and uuid even though its binVersion is latest since the FCV is
    // last-lts
    const primaryHealthlog = primary.getDB("local").system.healthlog;
    let query = {
        operation: "dbCheckStart",
        namespace: {$exists: false},
        collectionUUID: {$exists: false}
    };
    jsTestLog("checking primary dbCheckStart log");
    checkHealthLog(primaryHealthlog, query, 1);
    query = {
        operation: "dbCheckStop",
        namespace: {$exists: false},
        collectionUUID: {$exists: false}
    };
    jsTestLog("checking primary dbCheckStop log");
    checkHealthLog(primaryHealthlog, query, 1);

    // secondary should not see nss and uuid in health log
    const secondaryHealthlog = secondary.getDB("local").system.healthlog;
    query = {
        operation: "dbCheckStart",
        namespace: {$exists: false},
        collectionUUID: {$exists: false}
    };
    jsTestLog("checking secondary dbCheckStart log");
    checkHealthLog(secondaryHealthlog, query, 1);
    query = {
        operation: "dbCheckStop",
        namespace: {$exists: false},
        collectionUUID: {$exists: false}
    };
    jsTestLog("checking secondary dbCheckStop log");
    checkHealthLog(secondaryHealthlog, query, 1);

    assertForDbCheckErrorsForAllNodes(
        replSet, true /*assertForErrors*/, true /*assertForWarnings*/);

    // Now, clear the health logs again
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

testDbCheckStartStopEntries(latestLastLTSReplSet);

latestLastLTSReplSet.stopSet();
