/**
 * Tests that the upgradeDowngradeViewlessTimeseries oplog entry can be applied via applyOps.
 *
 * TODO(SERVER-114573): Remove once 9.0 becomes last-lts, as the oplog entry won't be
 * supported anymore.
 *
 * @tags: [
 *   featureFlagCreateViewlessTimeseriesCollections,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

if (lastLTSFCV != "8.0") {
    jsTest.log.info("Skipping test because last LTS FCV is no longer 8.0");
    quit();
}

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const adminDB = primary.getDB("admin");
const dbName = jsTestName();
const collName = "ts";
const testDB = primary.getDB(dbName);
const bucketsColl = testDB.getCollection("system.buckets." + collName);
const mainColl = testDB.getCollection(collName);

const timeseriesOptions = {
    timeField: "t",
    granularity: "seconds",
    bucketMaxSpanSeconds: 3600,
};

function makeCreateCmd(collectionName) {
    return {
        applyOps: [
            {
                op: "c",
                ns: dbName + ".$cmd",
                o: {
                    create: collectionName,
                    clusteredIndex: true,
                    timeseries: timeseriesOptions,
                },
            },
        ],
    };
}

function makeUpgradeDowngradeCmd(isUpgrade, uuid) {
    return {
        applyOps: [
            {
                op: "c",
                ns: dbName + ".$cmd",
                o: {upgradeDowngradeViewlessTimeseries: collName, isUpgrade: isUpgrade},
                ui: uuid,
            },
        ],
    };
}

// Phase 1: Test upgrade path at FCV 9.0 (feature flag enabled).
// Use applyOps to create a legacy (viewful) timeseries collection, then upgrade it.
{
    jsTest.log("Phase 1: Testing timeseries upgrade via applyOps at latest FCV");

    // Create a legacy system.buckets collection via applyOps.
    const fp1 = configureFailPoint(primary, "skipCreateTimeseriesVersionMismatchCheck");
    assert.commandWorked(testDB.adminCommand(makeCreateCmd("system.buckets." + collName)));
    fp1.off();
    const collUUID = bucketsColl.getUUID();
    assert.neq(null, bucketsColl.exists(), "system.buckets collection should exist after create");

    // Upgrade: viewful -> viewless via applyOps.
    assert.commandWorked(testDB.adminCommand(makeUpgradeDowngradeCmd(true, collUUID)));
    assert.eq(null, bucketsColl.exists(), "Expected no system.buckets collection after upgrade");
    assert.neq(null, mainColl.exists(), "Expected main collection after upgrade");

    // Clean up for phase 2.
    assert.commandWorked(testDB.runCommand({drop: collName}));
}

// Phase 2: Test downgrade path at FCV 8.0 (feature flag disabled).
// Use applyOps to create a viewless timeseries collection, then downgrade it.
{
    jsTest.log("Phase 2: Testing downgrade via applyOps at last-LTS FCV");

    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));

    // Create a viewless timeseries collection via applyOps.
    const fp2 = configureFailPoint(primary, "skipCreateTimeseriesVersionMismatchCheck");
    assert.commandWorked(testDB.adminCommand(makeCreateCmd(collName)));
    fp2.off();
    const collUUID = mainColl.getUUID();
    assert.neq(null, mainColl.exists(), "Main collection should exist after create");
    assert.eq(null, bucketsColl.exists(), "system.buckets should not exist for viewless collection");

    // Downgrade: viewless -> viewful via applyOps.
    assert.commandWorked(testDB.adminCommand(makeUpgradeDowngradeCmd(false, collUUID)));
    assert.neq(null, bucketsColl.exists(), "Expected system.buckets collection after downgrade");
}

rst.stopSet();
