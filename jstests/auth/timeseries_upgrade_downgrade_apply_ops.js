/**
 * Tests that the upgradeDowngradeViewlessTimeseries oplog entry applied via applyOps requires
 * the __system role when auth is enabled.
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

const keyFile = "jstests/libs/key1";
const rst = new ReplSetTest({nodes: 1, keyFile: keyFile});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const adminDB = primary.getDB("admin");

adminDB.createUser({user: "admin", pwd: "pwd", roles: ["root"]});
assert(adminDB.auth("admin", "pwd"));

const dbName = jsTestName();
const collName = "ts";
const testDB = primary.getDB(dbName);
const bucketsColl = testDB.getCollection("system.buckets." + collName);

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

// Create a legacy (viewful) timeseries collection via applyOps at FCV 9.0.
const fp = configureFailPoint(primary, "skipCreateTimeseriesVersionMismatchCheck");
assert.commandWorked(testDB.adminCommand(makeCreateCmd("system.buckets." + collName)));
fp.off();
const collUUID = bucketsColl.getUUID();

// A root user should not be able to applyOps an internal-only oplog entry.
assert.commandFailedWithCode(testDB.adminCommand(makeUpgradeDowngradeCmd(true, collUUID)), ErrorCodes.Unauthorized);

adminDB.logout();

// The __system role is required to applyOps internal-only oplog entries.
rst.asCluster(primary, () => {
    // Upgrade: viewful -> viewless via applyOps.
    assert.commandWorked(testDB.adminCommand(makeUpgradeDowngradeCmd(true, collUUID)));
    assert.eq(null, bucketsColl.exists(), "Expected no system.buckets collection after upgrade");

    // Downgrade: viewless -> viewful via applyOps.
    // First downgrade FCV so the feature flag is disabled (required for downgrade path).
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));
    assert.commandWorked(testDB.adminCommand(makeUpgradeDowngradeCmd(false, collUUID)));
    assert.neq(null, bucketsColl.exists(), "Expected system.buckets collection after downgrade");
});

rst.stopSet();
