/**
 * Tests that config.collections timeseries fields timeseriesBucketsMayHaveMixedSchemaData and
 * fixedBucketing are correctly populated during FCV upgrade and removed during FCV downgrade.
 *
 * Also verifies upgrade-idempotency for `fixedBucketing`: a collection created in FCV 8.0 (before
 * `fixedBucketing` existed) must conservatively adopt `fixedBucketing: false` after upgrading to
 * FCV 9.0, and re-creating that collection in FCV 9.0 while omitting `fixedBucketing` must succeed
 * and must not flip the field to true. Similarly verifies downgrade-idempotency: after a downgrade
 * strips the field, re-creating the collection while omitting `fixedBucketing` must succeed and
 * must not resurrect it.
 *
 * TODO (SERVER-116499): Remove this file once 9.0 becomes last LTS.
 *
 * @tags: [
 *   requires_timeseries,
 *   requires_sharding,
 * ]
 */
import {getTimeseriesCollForDDLOps} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
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

function createShardedTimeseries(collName, extraTsOpts = {}) {
    assert.commandWorked(
        st.s.adminCommand({
            shardCollection: dbName + "." + collName,
            key: {m: 1},
            timeseries: {timeField: "t", metaField: "m", ...extraTsOpts},
        }),
    );
}

function testFCVTransition(startFCV, targetFCV) {
    const isUpgrade = targetFCV === latestFCV;
    const direction = isUpgrade ? "upgrade" : "downgrade";
    jsTest.log.info(`Testing ${direction}: FCV ${startFCV} -> ${targetFCV}.`);

    assert.commandWorked(
        adminDB.runCommand({setFeatureCompatibilityVersion: startFCV, confirm: true}),
    );

    const collNameNoMixed = "ts_no_mixed_" + direction;
    const collNameMixed = "ts_mixed_" + direction;

    // Create two sharded timeseries collections. When fixedBucketing is enabled, give one
    // fixedBucketing=false and the other fixedBucketing=true so the downgrade strip is exercised
    // for both values.
    // NOTE: featureFlagFixedBucketingCatalog is FCV-gated, so this is only enabled at the latest FCV,
    // i.e. effectively only in the downgrade run, where collections are created before downgrading.
    const fixedBucketingEnabledAtCollCreation = FeatureFlagUtil.isPresentAndEnabled(
        db,
        "FixedBucketingCatalog",
    );
    createShardedTimeseries(
        collNameNoMixed,
        fixedBucketingEnabledAtCollCreation ? {fixedBucketing: false} : {},
    );
    createShardedTimeseries(
        collNameMixed,
        fixedBucketingEnabledAtCollCreation ? {fixedBucketing: true} : {},
    );

    // Set mixed-schema flag to true on one collection via collMod.
    assert.commandWorked(
        db.runCommand({collMod: collNameMixed, timeseriesBucketsMayHaveMixedSchemaData: true}),
    );

    // Verify config.collections state before conversion.
    if (isUpgrade) {
        // Legacy format: config.collections should NOT have timeseriesBucketsMayHaveMixedSchemaData
        // or fixedBucketing.
        for (const name of [collNameNoMixed, collNameMixed]) {
            const entry = getConfigEntry(name);
            assert(
                !entry.timeseriesFields.hasOwnProperty("timeseriesBucketsMayHaveMixedSchemaData"),
            );
            assert(!entry.timeseriesFields.hasOwnProperty("fixedBucketing"));
        }
    } else {
        // Viewless format: config.collections should have timeseriesBucketsMayHaveMixedSchemaData.
        const entryNoMixed = getConfigEntry(collNameNoMixed);
        assert.eq(false, entryNoMixed.timeseriesFields.timeseriesBucketsMayHaveMixedSchemaData);

        const entryMixed = getConfigEntry(collNameMixed);
        assert.eq(true, entryMixed.timeseriesFields.timeseriesBucketsMayHaveMixedSchemaData);

        if (fixedBucketingEnabledAtCollCreation) {
            assert.eq(false, entryNoMixed.timeseriesFields.fixedBucketing);
            assert.eq(true, entryMixed.timeseriesFields.fixedBucketing);
        }
    }

    // Read shard-local metadata to confirm collMod took effect.
    if (isUpgrade) {
        assert.eq(
            false,
            TimeseriesTest.bucketsMayHaveMixedSchemaData(db.getCollection(collNameNoMixed)),
        );
        assert.eq(
            true,
            TimeseriesTest.bucketsMayHaveMixedSchemaData(db.getCollection(collNameMixed)),
        );
    }

    jsTest.log.info(`Transitioning FCV: ${startFCV} -> ${targetFCV}`);
    assert.commandWorked(
        adminDB.runCommand({setFeatureCompatibilityVersion: targetFCV, confirm: true}),
    );

    // Verify config.collections state after conversion.
    if (isUpgrade) {
        // After upgrade to viewless: collNoMixed should have field=false, collMixed should have
        // field=true.
        const entryNoMixed = getConfigEntry(collNameNoMixed);
        assert.eq(false, entryNoMixed.timeseriesFields.timeseriesBucketsMayHaveMixedSchemaData);

        // collMixed should have field=true, mirroring the shard-local state set by collMod.
        const entryMixed = getConfigEntry(collNameMixed);
        assert.eq(true, entryMixed.timeseriesFields.timeseriesBucketsMayHaveMixedSchemaData);

        // On upgrade, collections created in FCV 8.0 (no fixedBucketing) adopt fixedBucketing=false
        // conservatively. When the flag is disabled the field stays absent.
        const fixedBucketingEnabledAfterUpgrade = FeatureFlagUtil.isPresentAndEnabled(
            db,
            "FixedBucketingCatalog",
        );

        if (fixedBucketingEnabledAfterUpgrade) {
            assert.eq(false, entryNoMixed.timeseriesFields.fixedBucketing);
            assert.eq(false, entryMixed.timeseriesFields.fixedBucketing);
        } else {
            assert(!entryNoMixed.timeseriesFields.hasOwnProperty("fixedBucketing"));
            assert(!entryMixed.timeseriesFields.hasOwnProperty("fixedBucketing"));
        }
    } else {
        // After downgrade to legacy: viewless-only fields must be stripped from config.collections.
        for (const name of [collNameNoMixed, collNameMixed]) {
            const entry = getConfigEntry(name);
            assert(
                !entry.timeseriesFields.hasOwnProperty("timeseriesBucketsMayHaveMixedSchemaData"),
            );
            assert(!entry.timeseriesFields.hasOwnProperty("fixedBucketing"));
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

// Check whether fixedBucketingCatalog feature is enabled, regardless of FCV (so that the result does not depend on the
// FCV left by the previous tests)
const fixedBucketingFlagEnabled = FeatureFlagUtil.isPresentAndEnabled(
    db,
    "FixedBucketingCatalog",
    /* ignoreFCV */ true,
);

// Test upgrade-idempotency for fixedBucketing: a collection created in FCV 8.0 must have
// fixedBucketing=false (not true) after upgrading to FCV 9.0, and re-creating it without
// specifying fixedBucketing must succeed (idempotent) and leave the field as false.
if (fixedBucketingFlagEnabled) {
    jsTest.log.info(
        "Testing fixedBucketing upgrade-idempotency: collection created in lastLTSFCV " +
            "must have fixedBucketing=false after upgrade, and re-creating it without " +
            "fixedBucketing must leave the field as false.",
    );

    const collName = "ts_fixedbucketing_idempotency";
    const tsOpts = {timeField: "t", metaField: "m"};

    // Start in lastLTSFCV (FCV 8.0) and create a sharded timeseries collection without
    // fixedBucketing (the field did not exist in FCV 8.0).
    assert.commandWorked(
        adminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
    );
    createShardedTimeseries(collName);

    // Upgrade to latestFCV. The upgrade path sets fixedBucketing=false conservatively on
    // existing collections, since we cannot prove their buckets are fixed.
    assert.commandWorked(
        adminDB.runCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
    );

    // Assert that fixedBucketing is false after upgrade (not true, not absent).
    const entryAfterUpgrade = getConfigEntry(collName);
    assert.eq(
        false,
        entryAfterUpgrade.timeseriesFields.fixedBucketing,
        "fixedBucketing must be false on a collection created in lastLTSFCV after upgrading to latestFCV",
        {timeseriesFieldsAfterUpgrade: entryAfterUpgrade.timeseriesFields},
    );

    // Re-create the collection via createCollection, omitting fixedBucketing. In FCV 9.0 a brand
    // new collection would default fixedBucketing=true, but this collection already exists with
    // fixedBucketing=false, so the idempotent re-creation must not flip it to true.
    assert.commandWorked(db.createCollection(collName, {timeseries: tsOpts}));

    const entryAfterRecreate = getConfigEntry(collName);
    assert.eq(
        false,
        entryAfterRecreate.timeseriesFields.fixedBucketing,
        "fixedBucketing must remain false after idempotent re-creation of a collection " +
            "that was originally created in lastLTSFCV",
        {timeseriesFieldsAfterRecreate: entryAfterRecreate.timeseriesFields},
    );

    // Re-creating the collection with explicit 'fixedBucketing: true' must fail.
    assert.commandFailedWithCode(
        db.createCollection(collName, {timeseries: {...tsOpts, fixedBucketing: true}}),
        ErrorCodes.NamespaceExists,
    );

    // Cleanup
    db[collName].drop();
}

// Test downgrade-idempotency for fixedBucketing:
// * Create the timeseries omitting fixedBucketing in FCV 9.0 => fixedBucketing set to true by default
// * Downgrade => fixedBucketing stripped
// * Re-create the timeseries omitting fixedBucketing => succeeds (idempotent), but does not bring back
//   'fixedBucketing: true'
if (fixedBucketingFlagEnabled) {
    jsTest.log.info(
        "Testing fixedBucketing downgrade-idempotency: re-creating a collection after a " +
            "downgrade stripped fixedBucketing must succeed and leave the field absent.",
    );

    const collName = "ts_fixedbucketing_downgrade_idempotency";
    const tsOpts = {timeField: "t", metaField: "m"};

    // Create a sharded timeseries collection in latestFCV; omitting fixedBucketing defaults the
    // stored value to true.
    assert.commandWorked(
        adminDB.runCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
    );
    createShardedTimeseries(collName);
    assert.eq(true, getConfigEntry(collName).timeseriesFields.fixedBucketing);

    // Downgrade to lastLTSFCV strips the field.
    assert.commandWorked(
        adminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
    );
    const entryAfterDowngrade = getConfigEntry(collName);
    assert(
        !entryAfterDowngrade.timeseriesFields.hasOwnProperty("fixedBucketing"),
        "fixedBucketing must be stripped after downgrading to lastLTSFCV",
        {timeseriesFieldsAfterDowngrade: entryAfterDowngrade.timeseriesFields},
    );

    // Re-create the collection omitting fixedBucketing: must succeed (idempotent) and leave the
    // field absent.
    assert.commandWorked(db.createCollection(collName, {timeseries: tsOpts}));
    const entryAfterRecreate = getConfigEntry(collName);
    assert(
        !entryAfterRecreate.timeseriesFields.hasOwnProperty("fixedBucketing"),
        "fixedBucketing must remain absent after idempotent re-creation following a downgrade",
        {timeseriesFieldsAfterRecreate: entryAfterRecreate.timeseriesFields},
    );

    // Cleanup
    db[collName].drop();
}

st.stop();
