/**
 * Tests `createCollection`, `collMod`, and `shardCollection` with the `fixedBucketing` timeseries
 * option during in-progress FCV transitions (downgrade and upgrade), on both replica set and
 * sharded cluster topologies.
 *
 * Key behaviors under test:
 *  - `createCollection` with an explicit `fixedBucketing` value fails with `InvalidOptions` while
 *    an FCV transition is in progress, because the flag reads as disabled at transitional FCV.
 *  - `createCollection` with `fixedBucketing` omitted succeeds and the field is not automatically
 *     set to true in stored options (because the flag is off).
 *  - `collMod` changing bucketing params during a downgrade, while the collection is still viewless
 *     (not yet converted) and still carries `fixedBucketing: true`, correctly flips the stored
 *     value to `false` (the flip logic reads the stored value directly and does not check the
 *     feature flag).
 *  - `shardCollection` (omitting `fixedBucketing`) on an existing viewless TS collection (which
 *     carries `fixedBucketing: true`) succeeds during a downgrade window and the 'fixedBucketing'
 *     field is stored in the global catalog.
 *  - `shardCollection` creating a new TS collection behaves like `createCollection` (explicit
 *    `fixedBucketing` rejected, omitted succeeds with the field absent).
 *  - After the upgrade completes, a collection created (as legacy) during the upgrade window is
 *    converted to viewless with `fixedBucketing: false`.
 *
 * The hang failpoints fire in the FCV transition's prepare phase, before the viewless<->viewful
 * conversion, so the pre-existing collection is still viewless when we exercise the downgrade window.
 *
 * TODO(SERVER-128768): Remove after 9.0 becomes last LTS.
 *
 * @tags: [
 *   requires_timeseries,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

if (lastLTSFCV != "8.0") {
    jsTest.log.info("Skipping test because last LTS FCV is no longer 8.0");
    quit();
}

// Return the stored `fixedBucketing` value from `listCollections`, or `undefined` when absent.
function getStoredFixedBucketing(db, name) {
    const colls = assert.commandWorked(
        db.runCommand({listCollections: 1, filter: {type: "timeseries", name}}),
    ).cursor.firstBatch;
    assert.eq(colls.length, 1, "expected exactly one timeseries collection", {colls});
    return colls[0].options.timeseries.fixedBucketing;
}

// Return the `fixedBucketing` value from `config.collections` for a sharded TS collection, or
// `undefined` when absent.
function getGlobalFixedBucketing(db, name) {
    const collNss = `${db.getName()}.${name}`;
    const entry = db.getSiblingDB("config").collections.findOne({_id: collNss});
    assert.neq(entry, null, "expected sharded collection in config.collections", {collNss});
    return entry.timeseriesFields.fixedBucketing;
}

// Configure the appropriate hang failpoint on `failpointConn`, start a `setFCV` Thread (to
// `targetFCV`) against the connection backing `db`, wait for the failpoint to be hit, run
// `body()`, then release the failpoint and join the Thread.
function withFCVTransitionPaused(db, failpointConn, targetFCV, body) {
    const fpName = targetFCV === lastLTSFCV ? "hangWhileDowngrading" : "hangWhileUpgrading";
    const fp = configureFailPoint(failpointConn, fpName);

    const fcvThread = new Thread(
        (host, version) => {
            const conn = new Mongo(host);
            assert.commandWorked(
                conn
                    .getDB("admin")
                    .runCommand({setFeatureCompatibilityVersion: version, confirm: true}),
            );
        },
        db.getMongo().host,
        targetFCV,
    );
    fcvThread.start();
    fp.wait();

    try {
        body();
    } finally {
        fp.off();
        fcvThread.join();
    }
}

// Assert the expected behavior when creating a new TS collection during a paused FCV transition:
// explicit `fixedBucketing` is rejected (`InvalidOptions`), and omitting it succeeds with the field
// absent. If `shardCollName` is provided, `shardCollection` (creating a new collection) is
// exercised the same way. `label` is a human-readable direction ("Downgrade" or "Upgrade") used in
// log messages. The caller is responsible for dropping `collName` and `shardCollName` afterward.
function checkNewCollectionBehavior(db, label, collName, shardCollName) {
    const adminDB = db.getSiblingDB("admin");
    const dbName = db.getName();

    jsTest.log.info(`${label}: createCollection with explicit fixedBucketing`);
    assert.commandFailedWithCode(
        db.createCollection("ts_explicit", {timeseries: {timeField: "t", fixedBucketing: true}}),
        ErrorCodes.InvalidOptions,
    );

    jsTest.log.info(`${label}: createCollection with fixedBucketing omitted`);
    assert.commandWorked(db.createCollection(collName, {timeseries: {timeField: "t"}}));
    assert.eq(
        getStoredFixedBucketing(db, collName),
        undefined,
        `expected fixedBucketing absent when omitted during ${label}`,
    );

    if (!shardCollName) {
        return;
    }

    jsTest.log.info(
        `${label}: shardCollection creating new TS collection, explicit fixedBucketing`,
    );
    assert.commandFailedWithCode(
        adminDB.runCommand({
            shardCollection: `${dbName}.ts_shard_explicit`,
            key: {hostId: 1},
            timeseries: {timeField: "t", metaField: "hostId", fixedBucketing: true},
        }),
        ErrorCodes.InvalidOptions,
    );

    jsTest.log.info(`${label}: shardCollection creating new TS collection, fixedBucketing omitted`);
    assert.commandWorked(
        adminDB.runCommand({
            shardCollection: `${dbName}.${shardCollName}`,
            key: {hostId: 1},
            timeseries: {timeField: "t", metaField: "hostId"},
        }),
    );
    assert.eq(
        getStoredFixedBucketing(db, shardCollName),
        undefined,
        `expected fixedBucketing absent when omitted via shardCollection during ${label}`,
    );
}

// Run the full test suite against the topology described by `db` and `failpointConn`. Set
// `isSharded` to true to exercise `shardCollection` paths in addition to `createCollection` and
// `collMod`.
function runTest(db, failpointConn, isSharded) {
    const adminDB = db.getSiblingDB("admin");
    const dbName = db.getName();
    const tsOptions = {
        timeField: "t",
        metaField: "hostId",
        bucketMaxSpanSeconds: 100,
        bucketRoundingSeconds: 100,
    };
    // collection created before the transition
    const collNamePreTransition = "ts_pre";
    // collection created during the transition window
    const collNameDuringTransition = "ts_intra";
    // collection created (via shardCollection) during the transition window (sharded case only)
    const collNameShardDuringTransition = "ts_intra_shard";

    // -----------------------------------------------------------------------
    // Downgrade window
    // -----------------------------------------------------------------------
    jsTest.log.info("=== Downgrade window ===");

    assert.commandWorked(db.dropDatabase());
    if (isSharded) {
        assert.commandWorked(adminDB.runCommand({enableSharding: dbName}));
    }

    // Create a viewless TS collection at latest FCV; `fixedBucketing` defaults to true.
    assert.commandWorked(db.createCollection(collNamePreTransition, {timeseries: tsOptions}));
    assert.eq(
        getStoredFixedBucketing(db, collNamePreTransition),
        true,
        "expected fixedBucketing: true before transition",
    );

    withFCVTransitionPaused(db, failpointConn, lastLTSFCV, () => {
        // The hang failpoint fires before the viewless->viewful conversion, so the collection is
        // still viewless and `fixedBucketing` is still true at this point.
        assert.eq(
            getStoredFixedBucketing(db, collNamePreTransition),
            true,
            "collection must still carry fixedBucketing: true at the start of the downgrade window",
        );

        if (isSharded) {
            // Even though the feature flag is disabled at transitional FCV, shardCollection
            // (omitting 'fixedBucketing') on an existing viewless collection succeeds and
            // `fixedBucketing` is preserved in the global catalog.
            jsTest.log.info(
                "Downgrade: shardCollection (omitting 'fixedBucketing') on existing viewless collection",
            );
            const collNss = `${dbName}.${collNamePreTransition}`;
            assert.commandWorked(adminDB.runCommand({shardCollection: collNss, key: {hostId: 1}}));
            assert.eq(
                getGlobalFixedBucketing(db, collNamePreTransition),
                true,
                "expected fixedBucketing: true preserved in config.collections after shardCollection",
            );
            assert.eq(
                getStoredFixedBucketing(db, collNamePreTransition),
                true,
                "expected fixedBucketing: true still visible via listCollections after shardCollection",
            );
        }

        // collMod changing bucketing params on the pre-existing viewless TS collection (still
        // viewless at this point in the downgrade) must flip fixedBucketing from true to false even
        // if the feature flag is already off.
        jsTest.log.info("Downgrade: collMod changes fixedBucketing true -> false");
        assert.commandWorked(
            db.runCommand({
                collMod: collNamePreTransition,
                timeseries: {bucketMaxSpanSeconds: 200, bucketRoundingSeconds: 200},
            }),
        );
        assert.eq(
            getStoredFixedBucketing(db, collNamePreTransition),
            false,
            "expected collMod to flip fixedBucketing to false during downgrade window",
        );

        // Check behavior of createCollection and shardCollection when creating a new collection
        // during transitional FCV.
        checkNewCollectionBehavior(
            db,
            "Downgrade",
            collNameDuringTransition,
            isSharded ? collNameShardDuringTransition : null,
        );
        assert(db[collNameDuringTransition].drop());
        if (isSharded) {
            assert(db[collNameShardDuringTransition].drop());
        }
    });

    // -----------------------------------------------------------------------
    // Upgrade window
    // -----------------------------------------------------------------------
    jsTest.log.info("=== Upgrade window ===");

    // System is now downgraded (by the previous step); set up a fresh database for the upgrade test.
    assert.commandWorked(db.dropDatabase());
    if (isSharded) {
        assert.commandWorked(adminDB.runCommand({enableSharding: dbName}));
    }

    withFCVTransitionPaused(db, failpointConn, latestFCV, () => {
        // Check behavior of createCollection and shardCollection when creating a new collection
        // during transitional FCV.
        checkNewCollectionBehavior(
            db,
            "Upgrade",
            collNameDuringTransition,
            isSharded ? collNameShardDuringTransition : null,
        );
        // Leave collNameDuringTransition (and collNameShardDuringTransition) alive so we can
        // check them after the upgrade completes below.
    });

    // Once the upgrade is completed, collections created (as legacy) during kUpgrading are
    // converted to viewless with fixedBucketing: false.
    jsTest.log.info("Post-upgrade: verifying fixedBucketing: false on upgraded collections");
    assert.eq(
        getStoredFixedBucketing(db, collNameDuringTransition),
        false,
        "expected fixedBucketing: false after upgradeAllTimeseriesToViewless",
    );
    assert(db[collNameDuringTransition].drop());

    if (isSharded) {
        // Verify `fixedBucketing: false` in both the local and global catalog.
        assert.eq(
            getStoredFixedBucketing(db, collNameShardDuringTransition),
            false,
            "expected fixedBucketing: false in local catalog for sharded collection after upgrade",
        );
        assert.eq(
            getGlobalFixedBucketing(db, collNameShardDuringTransition),
            false,
            "expected fixedBucketing: false in global catalog after upgrade",
        );
        assert(db[collNameShardDuringTransition].drop());
    }
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
