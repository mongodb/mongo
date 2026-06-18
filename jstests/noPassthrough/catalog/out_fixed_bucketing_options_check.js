/**
 * Tests that $out to an existing timeseries collection succeeds even when the target's
 * 'fixedBucketing' option changes between $out's snapshot and its guarded rename.
 *
 * 'fixedBucketing' records whether the bucketing parameters have remained unchanged since the
 * collection's creation, and is managed by the system rather than the user. Two legitimate,
 * non-user mutations can change it between $out's snapshot and its final rename:
 *   1. A concurrent $out to the same target: the replacement collection is created with the
 *      creation-time default ('fixedBucketing: true'), so a concurrent $out whose snapshot
 *      captured a different value sees a mismatch.
 *   2. An FCV downgrade: the SERVER-126822 pass strips 'fixedBucketing' from existing
 *      collections; a subsequent upgrade does not re-add it to existing ones.
 *
 * The fix is in checkTargetCollectionOptionsMatch (rename_collection.cpp): 'fixedBucketing' is
 * excluded from the shape comparison alongside the existing 'uuid' exclusion.
 *
 * NOTE: 'fixedBucketing' can be automatically changed by a user-initiated collMod request that
 * changes bucketing parameters. But such a user-initiated mutation is still detected exactly
 * because other parameters are also changed.
 *
 * This test lives in noPassthrough/catalog because it pins local rename-with-options-precondition
 * semantics owned by server-catalog-and-routing-shard-catalog. $out is the driver because it is
 * the only client that reaches the rename options guard — the public renameCollection command
 * cannot (its IDL is strict, the forwarding layer never sets expectedCollectionOptions, and the
 * internal commands require ActionType::internal).
 *
 * @tags: [
 *   requires_fcv_90,
 *   requires_replication,
 *   requires_timeseries,
 *   featureFlagFixedBucketingCatalog,
 *   featureFlagCreateViewlessTimeseriesCollections,
 * ]
 */

import {waitForCurOpByFailPointNoNS} from "jstests/libs/curop_helpers.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const testDB = primary.getDB("out_fixed_bucketing_options_check");

const timeField = "t";
const metaField = "m";
const srcCollName = "src";
const outCollName = "out_coll";

// Insert a handful of documents into the source collection.
function setupSource() {
    testDB[srcCollName].drop();
    const docs = [];
    for (let i = 0; i < 5; i++) {
        docs.push({[timeField]: new Date(), [metaField]: i, v: i});
    }
    assert.commandWorked(testDB[srcCollName].insertMany(docs));
}

// Run $out to outCollName with an explicit timeseries spec, hung at the rename failpoint.
// The failpoint is configured to only pause $out operations tagged with comment "pauseHere",
// so a concurrent $out without that comment can run to completion unimpeded.
// Calls func() while $out is suspended, then releases and returns the aggregate result.
function runOutWithHook(func) {
    assert.commandWorked(
        primary.getDB("admin").runCommand({
            configureFailPoint: "outWaitBeforeTempCollectionRename",
            mode: "alwaysOn",
            data: {comment: "pauseHere"},
        }),
    );

    const thread = new Thread(
        (host, dbName, srcName, outName, timeField, metaField) => {
            const conn = new Mongo(host);
            return conn.getDB(dbName).runCommand({
                aggregate: srcName,
                pipeline: [
                    {
                        $out: {
                            db: dbName,
                            coll: outName,
                            timeseries: {timeField: timeField, metaField: metaField},
                        },
                    },
                ],
                cursor: {},
                comment: "pauseHere",
            });
        },
        primary.host,
        testDB.getName(),
        srcCollName,
        outCollName,
        timeField,
        metaField,
    );
    thread.start();

    // Wait until $out is suspended at the rename failpoint.
    waitForCurOpByFailPointNoNS(testDB, "outWaitBeforeTempCollectionRename");

    func();

    assert.commandWorked(
        primary
            .getDB("admin")
            .runCommand({configureFailPoint: "outWaitBeforeTempCollectionRename", mode: "off"}),
    );

    return thread.returnData();
}

// Verify the output collection has the expected document count and is a timeseries collection.
function verifyOutput() {
    const infos = testDB.getCollectionInfos({name: outCollName});
    assert.eq(infos.length, 1, `Expected 1 collection info for ${outCollName}: ${tojson(infos)}`);
    assert.eq(infos[0].type, "timeseries", `Expected timeseries type: ${tojson(infos[0])}`);
    assert.eq(testDB[outCollName].countDocuments({}), 5, "Unexpected document count after $out");
}

// ---------------------------------------------------------------------------
// Case 1: a concurrent $out B completes while $out A is suspended at the rename
// failpoint. $out B installs a new target with fixedBucketing: true (the creation-
// time default), which differs from the false value $out A snapshotted.
// ---------------------------------------------------------------------------
jsTestLog("Case 1: concurrent $out changes fixedBucketing on the target");
{
    setupSource();

    // Create target with fixedBucketing: false so $out A's snapshot captures it.
    testDB[outCollName].drop();
    assert.commandWorked(
        testDB.createCollection(outCollName, {
            timeseries: {timeField, metaField, fixedBucketing: false},
        }),
    );

    const res = runOutWithHook(() => {
        // While $out A is suspended, run $out B to completion. It carries a different
        // comment so it bypasses the failpoint and replaces the target with a new
        // collection that has fixedBucketing: true (the creation-time default).
        assert.commandWorked(
            testDB.runCommand({
                aggregate: srcCollName,
                pipeline: [
                    {
                        $out: {
                            db: testDB.getName(),
                            coll: outCollName,
                            timeseries: {timeField, metaField},
                        },
                    },
                ],
                cursor: {},
                comment: "doNotPause",
            }),
        );
    });

    assert.commandWorked(res, `Case 1 failed: ${tojson(res)}`);
    verifyOutput();
}

// ---------------------------------------------------------------------------
// Case 2: an FCV downgrade + upgrade cycle strips fixedBucketing from the live
// target while $out holds the pre-downgrade snapshot.
// Note: a single downgrade (without re-upgrade) is intentionally not tested
// here — the viewless→viewful format change trips checkTimeseriesUpgradeDowngrade
// (error 485) before the options comparison. We test the full down+up cycle
// because in that interleaving the format reverts to viewless but fixedBucketing
// is not re-added, so the format-change guard does not fire.
// ---------------------------------------------------------------------------
jsTestLog("Case 2: FCV downgrade+upgrade strips fixedBucketing from the live target");
{
    setupSource();

    // Create target normally — under latest FCV it gets fixedBucketing: true.
    testDB[outCollName].drop();
    assert.commandWorked(
        testDB.createCollection(outCollName, {
            timeseries: {timeField, metaField},
        }),
    );
    const infoBefore = testDB.getCollectionInfos({name: outCollName})[0];
    assert(
        infoBefore.options.timeseries.fixedBucketing,
        `Expected fixedBucketing: true on target before $out: ${tojson(infoBefore)}`,
    );

    const res = runOutWithHook(() => {
        // Downgrade strips fixedBucketing from the target; upgrade does not re-add it.
        assert.commandWorked(
            primary
                .getDB("admin")
                .runCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
        );
        assert.commandWorked(
            primary
                .getDB("admin")
                .runCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
        );

        // Confirm the strip happened.
        const infoAfter = testDB.getCollectionInfos({name: outCollName})[0];
        assert(
            !infoAfter.options.timeseries.hasOwnProperty("fixedBucketing"),
            `Expected fixedBucketing stripped after FCV cycle: ${tojson(infoAfter)}`,
        );
    });

    assert.commandWorked(res, `Case 2 failed: ${tojson(res)}`);
    verifyOutput();
}

rst.stopSet();
