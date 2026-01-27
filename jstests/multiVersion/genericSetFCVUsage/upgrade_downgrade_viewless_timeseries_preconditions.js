/**
 * TODO (SERVER-116499): Remove this file once 9.0 becomes last LTS.
 *
 * Tests preconditions for the upgradeDowngradeViewlessTimeseries cluster command.
 *
 * @tags: [
 *   requires_timeseries,
 *   requires_sharding,
 *   featureFlagCreateViewlessTimeseriesCollections,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {getTimeseriesBucketsColl} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";

if (lastLTSFCV != "8.0") {
    jsTest.log.info("Skipping test because last LTS FCV is no longer 8.0");
    quit();
}

const st = new ShardingTest({shards: 2, rs: {nodes: 1}});

const dbName = jsTestName();
const db = st.s.getDB(dbName);
const adminDB = st.s.getDB("admin");

assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

/**
 * Tests preconditions for the downgrade path (downgradeToLegacy).
 * With feature flag enabled, collections are created in viewless format.
 */
function testDowngradeErrors() {
    jsTest.log.info("=== Testing Downgrade Error Scenarios ===");

    {
        jsTest.log.info("Downgrade: Non-existent namespace should fail with NamespaceNotFound");
        assert.commandFailedWithCode(
            db.runCommand({
                upgradeDowngradeViewlessTimeseries: "nonExistentDowngrade",
                mode: "downgradeToLegacy",
            }),
            ErrorCodes.NamespaceNotFound,
        );
    }

    {
        jsTest.log.info("Downgrade: Non-timeseries collection should fail with IllegalOperation");
        const regularCollName = "regularCollDowngrade";
        assert.commandWorked(db.createCollection(regularCollName));
        assert.commandFailedWithCode(
            db.runCommand({
                upgradeDowngradeViewlessTimeseries: regularCollName,
                mode: "downgradeToLegacy",
            }),
            ErrorCodes.IllegalOperation,
        );
        assert(db.getCollection(regularCollName).drop());
    }
}

/**
 * Tests preconditions for the upgrade path (upgradeToViewless).
 * Need to set FCV to lastLTS first to create legacy format collections,
 * then set FCV back to latest to enable the upgrade command.
 */
function testUpgradeErrors() {
    jsTest.log.info("=== Testing Upgrade Error Scenarios ===");

    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));

    const regularCollName = "regularCollUpgrade";
    assert.commandWorked(db.createCollection(regularCollName));

    const tsCollName = "tsCollUpgrade";
    assert.commandWorked(db.createCollection(tsCollName, {timeseries: {timeField: "t"}}));

    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));

    {
        jsTest.log.info("Upgrade: Non-existent namespace should fail with NamespaceNotFound");
        assert.commandFailedWithCode(
            db.runCommand({
                upgradeDowngradeViewlessTimeseries: "nonExistentUpgrade",
                mode: "upgradeToViewless",
            }),
            ErrorCodes.NamespaceNotFound,
        );
    }

    {
        jsTest.log.info("Upgrade: Non-timeseries collection should fail with IllegalOperation");
        assert.commandFailedWithCode(
            db.runCommand({
                upgradeDowngradeViewlessTimeseries: regularCollName,
                mode: "upgradeToViewless",
            }),
            ErrorCodes.IllegalOperation,
        );
        assert(db.getCollection(regularCollName).drop());
    }

    {
        jsTest.log.info("Upgrade: Using system.buckets namespace should fail");
        const bucketsCollName = getTimeseriesBucketsColl(tsCollName);
        assert.commandFailedWithCode(
            db.runCommand({
                upgradeDowngradeViewlessTimeseries: bucketsCollName,
                mode: "upgradeToViewless",
            }),
            ErrorCodes.InvalidNamespace,
        );
        assert(db.getCollection(tsCollName).drop());
    }
}

// Run the tests
testDowngradeErrors();
testUpgradeErrors();

st.stop();
