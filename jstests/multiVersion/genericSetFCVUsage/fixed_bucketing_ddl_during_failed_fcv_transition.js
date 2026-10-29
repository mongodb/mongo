/**
 * Tests `fixedBucketing` behavior on timeseries collections after **failed** FCV transitions
 * (setFCV returns an error, leaving the cluster in kDowngrading or kUpgrading), on both replica
 * set and sharded cluster topologies.
 *
 * Key behaviors under test:
 *  - Downgrade -> Fail -> Rollback: `fixedBucketing` is preserved on pre-existing collections;
 *    a `collMod` during kDowngrading still flips `fixedBucketing` true→false; a new collection
 *    created while the flag was off is in legacy (viewful) format and gets `fixedBucketing: false`
 *    upon recovery, when `upgradeAllTimeseriesToViewless` converts it.
 *  - Downgrade -> Fail -> Retry: `fixedBucketing` is stripped.
 *  - Upgrade -> Fail -> Rollback: `fixedBucketing` stays absent
 *  - Upgrade -> Fail -> Retry:  `fixedBucketing: false`.
 *
 * NOTE: The `failDowngrading` / `failUpgrading` failpoints fire in the FCV transition's prepare
 * phase, before any viewless<->viewful collection conversion.
 *
 * TODO(SERVER-128768): Remove after 9.0 becomes last LTS.
 *
 * @tags: [
 *   requires_timeseries,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {getTimeseriesCollForDDLOps} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";

if (lastLTSFCV != "8.0") {
    jsTest.log.info("Skipping test because last LTS FCV is no longer 8.0");
    quit();
}

// Return the stored `fixedBucketing` value from the local catalog, or `undefined` when absent.
function getStoredFixedBucketing(db, name) {
    const trackedName = getTimeseriesCollForDDLOps(db, name);
    const colls = assert.commandWorked(
        db.runCommand({listCollections: 1, filter: {name: trackedName}}),
    ).cursor.firstBatch;
    assert.eq(colls.length, 1, "expected exactly one collection", {colls});
    return colls[0].options.timeseries?.fixedBucketing;
}

// Return the `fixedBucketing` value of a sharded timeseries collection from the global catalog, or
// `undefined` when absent.
function getGlobalFixedBucketing(db, name) {
    const trackedName = getTimeseriesCollForDDLOps(db, name);
    const collNss = `${db.getName()}.${trackedName}`;
    const entry = db.getSiblingDB("config").collections.findOne({_id: collNss});
    assert.neq(entry, null, "expected sharded collection in config.collections", {collNss});
    return entry.timeseriesFields?.fixedBucketing;
}

// Assert fixedBucketing values in both local and global catalog (when isSharded is true).
// `expectedByName` maps each collection name to its expected fixedBucketing value.
function assertFixedBucketing(db, isSharded, expectedByName) {
    for (const [name, expected] of Object.entries(expectedByName)) {
        assert.eq(
            getStoredFixedBucketing(db, name),
            expected,
            `expected fixedBucketing ${expected} in local catalog for ${name}`,
        );
        if (isSharded) {
            assert.eq(
                getGlobalFixedBucketing(db, name),
                expected,
                `expected fixedBucketing ${expected} in config.collections for ${name}`,
            );
        }
    }
}

// Convenience wrapper: assert that all test collections share the same `expected` value.
function assertAllFixedBucketing(db, isSharded, expected) {
    const names = ["ts_pre", "ts_collmod", "ts_new"];
    if (isSharded) {
        names.push("ts_to_shard");
    }
    assertFixedBucketing(db, isSharded, Object.fromEntries(names.map((n) => [n, expected])));
}

// Create a timeseries collection, sharded or unsharded depending on `useShardCollection`.
function createTimeseries(db, collName, useShardCollection, extraTsOpts = {}) {
    const tsOpts = {timeField: "t", metaField: "hostId", ...extraTsOpts};
    if (useShardCollection) {
        const adminDB = db.getSiblingDB("admin");
        const dbName = db.getName();
        assert.commandWorked(
            adminDB.runCommand({
                shardCollection: `${dbName}.${collName}`,
                key: {hostId: 1},
                timeseries: tsOpts,
            }),
        );
    } else {
        assert.commandWorked(db.createCollection(collName, {timeseries: tsOpts}));
    }
}

// Create the pre-existing collections for each sub-test:
//   ts_pre      — untouched during the failure window (baseline)
//   ts_collmod  — will have its granularity changed to "hours" via collMod during the window
//   ts_to_shard — [sharded only] created unsharded; will be sharded during the window
function setupCollections(db, isSharded) {
    createTimeseries(db, "ts_pre", isSharded);
    createTimeseries(db, "ts_collmod", isSharded, {granularity: "seconds"});
    if (isSharded) {
        createTimeseries(db, "ts_to_shard", false /* useShardCollection */);
    }
}

// Run a set of DDL operations while FCV is in a transitional (kDowngrading or kUpgrading) state:
//   collMod ts_collmod: granularity "seconds" → "hours"
//   [sharded only] shardCollection ts_to_shard
//   create ts_new
function runDDLDuringWindow(db, isSharded) {
    const adminDB = db.getSiblingDB("admin");
    const dbName = db.getName();

    assert.commandWorked(
        db.runCommand({collMod: "ts_collmod", timeseries: {granularity: "hours"}}),
    );

    if (isSharded) {
        assert.commandWorked(
            adminDB.runCommand({shardCollection: `${dbName}.ts_to_shard`, key: {hostId: 1}}),
        );
    }

    createTimeseries(db, "ts_new", isSharded);
}

// Drop all collections created by setupCollections / runDDLDuringWindow.
function dropTestCollections(db, isSharded) {
    for (const name of ["ts_pre", "ts_collmod", "ts_new"]) {
        assert(db[name].drop());
    }
    if (isSharded) {
        assert(db["ts_to_shard"].drop());
    }
}

function runTest(db, failpointConn, isSharded) {
    const adminDB = db.getSiblingDB("admin");
    const dbName = db.getName();

    assert.commandWorked(db.dropDatabase());
    if (isSharded) {
        assert.commandWorked(adminDB.runCommand({enableSharding: dbName}));
    }

    // -----------------------------------------------------------------------
    // Downgrade -> Fail -> Rollback
    // -----------------------------------------------------------------------
    jsTest.log.info("=== Downgrade -> Fail -> Rollback ===");

    setupCollections(db, isSharded);

    configureFailPoint(failpointConn, "failDowngrading", {}, {times: 1});
    assert.commandFailed(
        adminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
    );

    runDDLDuringWindow(db, isSharded);

    // Rollback: go back to latestFCV
    jsTest.log.info("recovery setFCV(latestFCV)");
    assert.commandWorked(
        adminDB.runCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
    );

    // Expected results:
    // - ts_pre: untouched; original fixedBucketing value (true) preserved.
    // - ts_collmod: collMod flipped fixedBucketing to false.
    // - ts_new: created in legacy (viewful) format while the flag was off; downgrade rollback
    //           converts it to viewless and sets fixedBucketing to false.
    // - ts_to_shard: created before downgrade windows and sharded during kDowngrading =>
    //                original fixedBucketing value (true) preserved.
    const expected = {ts_pre: true, ts_collmod: false, ts_new: false};
    if (isSharded) {
        expected.ts_to_shard = true;
    }
    assertFixedBucketing(db, isSharded, expected);

    dropTestCollections(db, isSharded);

    // -----------------------------------------------------------------------
    // Downgrade -> Fail -> Retry
    // -----------------------------------------------------------------------
    jsTest.log.info("=== Downgrade -> Fail -> Retry ===");

    setupCollections(db, isSharded);

    configureFailPoint(failpointConn, "failDowngrading", {}, {times: 1});
    assert.commandFailed(
        adminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
    );

    runDDLDuringWindow(db, isSharded);

    // Retry: the downgrade now succeeds.
    jsTest.log.info("retry setFCV(lastLTS)");
    assert.commandWorked(
        adminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
    );

    // After a successful downgrade fixedBucketing is stripped (and all collections are in legacy
    // format)
    assertAllFixedBucketing(db, isSharded, undefined);

    dropTestCollections(db, isSharded);

    // -----------------------------------------------------------------------
    // Upgrade -> Fail -> Rollback
    // -----------------------------------------------------------------------
    jsTest.log.info("=== Upgrade -> Fail -> Rollback ===");

    // FCV = lastLTS from previous test. Collections are created in legacy format (no
    // fixedBucketing)
    setupCollections(db, isSharded);

    // Start upgrade and fail it.
    configureFailPoint(failpointConn, "failUpgrading", {}, {times: 1});
    assert.commandFailed(
        adminDB.runCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
    );

    // Run test DDL operations while in kUpgrading state.
    runDDLDuringWindow(db, isSharded);

    // Rollback: setFCV(lastLTS) succeeds
    jsTest.log.info("rollback setFCV(lastLTS)");
    assert.commandWorked(
        adminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
    );

    // All collections stay in legacy format and fixedBucketing stays unset for all of them.
    assertAllFixedBucketing(db, isSharded, undefined);

    dropTestCollections(db, isSharded);

    // -----------------------------------------------------------------------
    // Upgrade -> Fail -> Retry
    // -----------------------------------------------------------------------
    jsTest.log.info("=== Upgrade -> Fail -> Retry ===");

    // FCV = lastLTS from previous test. Collections are created in legacy format (no
    // fixedBucketing)
    setupCollections(db, isSharded);

    configureFailPoint(failpointConn, "failUpgrading", {}, {times: 1});
    assert.commandFailed(
        adminDB.runCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
    );

    // Run test DDL operations while in kUpgrading state.
    runDDLDuringWindow(db, isSharded);

    // Retry: the upgrade now succeeds.
    jsTest.log.info("retry setFCV(latestFCV)");
    assert.commandWorked(
        adminDB.runCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
    );

    // All (legacy) timeseries are converted to viewless and fixedBucketing is set to false for all
    // of them.
    assertAllFixedBucketing(db, isSharded, false);

    dropTestCollections(db, isSharded);
}

// Replica set.
{
    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();
    runTest(rst.getPrimary().getDB(jsTestName()), rst.getPrimary(), false /* isSharded */);
    rst.stopSet();
}

// Sharded cluster.
{
    const st = new ShardingTest({shards: 1, rs: {nodes: 1}});
    runTest(st.s.getDB(jsTestName()), st.rs0.getPrimary(), true /* isSharded */);
    st.stop();
}
