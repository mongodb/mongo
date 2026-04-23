/**
 * Tests that config.collections timeseries fields timeseriesBucketsMayHaveMixedSchemaData is
 * correctly populated during FCV upgrade and removed during FCV downgrade.
 *
 * TODO (SERVER-116499): Remove this file once 9.0 becomes last LTS.
 *
 * @tags: [
 *   requires_timeseries,
 *   requires_sharding,
 *   featureFlagCreateViewlessTimeseriesCollections,
 * ]
 */
import {getTimeseriesCollForDDLOps} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

if (lastLTSFCV != "8.0") {
    jsTest.log.info("Skipping test because last LTS FCV is no longer 8.0");
    quit();
}

const st = new ShardingTest({shards: 1, rs: {nodes: 1}});
const dbName = jsTestName();
const db = st.s.getDB(dbName);
const adminDB = st.s.getDB("admin");
const configDB = st.s.getDB("config");

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));

// Returns the config.collections entry for the given timeseries collection.
function getConfigEntry(collName) {
    const trackedNs = dbName + "." + getTimeseriesCollForDDLOps(db, collName);
    const entry = configDB.collections.findOne({_id: trackedNs});
    assert.neq(null, entry, "Expected config.collections entry for " + trackedNs);
    return entry;
}

function createShardedTimeseries(collName) {
    assert.commandWorked(
        st.s.adminCommand({
            shardCollection: dbName + "." + collName,
            key: {m: 1},
            timeseries: {timeField: "t", metaField: "m"},
        }),
    );
}

function testFCVTransition(startFCV, targetFCV) {
    const isUpgrade = targetFCV === latestFCV;
    const direction = isUpgrade ? "upgrade" : "downgrade";
    jsTest.log.info(`Testing ${direction}: FCV ${startFCV} -> ${targetFCV}.`);

    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: startFCV, confirm: true}));

    // Create two sharded timeseries collections.
    const collNameNoMixed = "ts_no_mixed_" + direction;
    const collNameMixed = "ts_mixed_" + direction;

    createShardedTimeseries(collNameNoMixed);
    createShardedTimeseries(collNameMixed);

    // Set mixed-schema flag to true on one collection via collMod.
    assert.commandWorked(db.runCommand({collMod: collNameMixed, timeseriesBucketsMayHaveMixedSchemaData: true}));

    // Verify config.collections state before conversion.
    if (isUpgrade) {
        // Legacy format: config.collections should NOT have timeseriesBucketsMayHaveMixedSchemaData.
        for (const name of [collNameNoMixed, collNameMixed]) {
            const entry = getConfigEntry(name);
            assert(!entry.timeseriesFields.hasOwnProperty("timeseriesBucketsMayHaveMixedSchemaData"));
        }
    } else {
        // Viewless format: config.collections should have timeseriesBucketsMayHaveMixedSchemaData.
        const entryNoMixed = getConfigEntry(collNameNoMixed);
        assert.eq(false, entryNoMixed.timeseriesFields.timeseriesBucketsMayHaveMixedSchemaData);

        const entryMixed = getConfigEntry(collNameMixed);
        assert.eq(true, entryMixed.timeseriesFields.timeseriesBucketsMayHaveMixedSchemaData);
    }

    // Read shard-local metadata to confirm collMod took effect.
    if (isUpgrade) {
        assert.eq(false, TimeseriesTest.bucketsMayHaveMixedSchemaData(db.getCollection(collNameNoMixed)));
        assert.eq(true, TimeseriesTest.bucketsMayHaveMixedSchemaData(db.getCollection(collNameMixed)));
    }

    jsTest.log.info(`Transitioning FCV: ${startFCV} -> ${targetFCV}`);
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: targetFCV, confirm: true}));

    // Verify config.collections state after conversion.
    if (isUpgrade) {
        // After upgrade to viewless: collNoMixed should have field=false, collMixed should have field=true.
        const entryNoMixed = getConfigEntry(collNameNoMixed);
        assert.eq(false, entryNoMixed.timeseriesFields.timeseriesBucketsMayHaveMixedSchemaData);

        // collMixed should have field=true, mirroring the shard-local state set by collMod.
        const entryMixed = getConfigEntry(collNameMixed);
        assert.eq(true, entryMixed.timeseriesFields.timeseriesBucketsMayHaveMixedSchemaData);
    } else {
        // After downgrade to legacy: config.collections should not have timeseriesBucketsMayHaveMixedSchemaData.
        for (const name of [collNameNoMixed, collNameMixed]) {
            const entry = getConfigEntry(name);
            assert(!entry.timeseriesFields.hasOwnProperty("timeseriesBucketsMayHaveMixedSchemaData"));
        }
    }

    // Cleanup
    db[collNameNoMixed].drop();
    db[collNameMixed].drop();
}

// Test upgrade (legacy -> viewless): verifies fields are populated from shard local catalog.
testFCVTransition(lastLTSFCV, latestFCV);

// Test downgrade (viewless -> legacy): verifies fields are not present.
testFCVTransition(latestFCV, lastLTSFCV);

st.stop();
