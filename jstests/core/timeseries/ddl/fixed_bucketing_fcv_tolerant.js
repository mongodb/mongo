/**
 * Lightweight FCV-tolerant test for `fixedBucketing` DDL operations.
 *
 * Asserts only invariants that hold regardless of whether `featureFlagFixedBucketingCatalog` is
 * currently enabled:
 *  - `createCollection` without an explicit `fixedBucketing` value always succeeds.
 *  - `collMod` changing bucketing parameters always succeeds.
 *  - `listCollections` reports `fixedBucketing` as either `false` or absent after the `collMod`.
 *
 * Stronger assertions (the exact stored value) are made only when `isViewlessTimeseriesOnlySuite()`
 * is true (stable FCV suite).
 *
 * TODO(SERVER-128768): Remove or simplify once 9.0 becomes last LTS and
 * featureFlagFixedBucketingCatalog is always on.
 *
 * @tags: [
 *   requires_timeseries,
 * ]
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {isViewlessTimeseriesOnlySuite} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";

if (!FeatureFlagUtil.isPresentAndEnabled(db, "FixedBucketingCatalog", true /* ignoreFCV */)) {
    jsTest.log.info("Skipping test: featureFlagFixedBucketingCatalog is not enabled at startup");
    quit();
}

const testDB = db.getSiblingDB(jsTestName());
const collName = "ts";

assert.commandWorked(testDB.dropDatabase());

// Create a viewless TS collection with custom bucketing params. `fixedBucketing` defaults
// to `true` when the feature is enabled (the params are unchanged since creation).
assert.commandWorked(
    testDB.createCollection(collName, {
        timeseries: {timeField: "t", bucketMaxSpanSeconds: 100, bucketRoundingSeconds: 100},
    }),
);

const fixedBucketingStrongAsserts = isViewlessTimeseriesOnlySuite(testDB);

// Test that createCollection succeeds, then read back the collection and check that:
// - fixedBucketing is absent if the timeseries is legacy
// - fixedBucketing is true if the timeseries is viewless and the suite is FCV-stable.
// - fixedBucketing is present and a boolean (either true or false) if the timeseries is viewless
//   and the suite is an FCV upgrade/downgrade one.
(function assertFixedBucketingAfterCreate() {
    const colls = assert.commandWorked(
        testDB.runCommand({listCollections: 1, filter: {type: "timeseries", name: collName}}),
    ).cursor.firstBatch;
    assert.eq(colls.length, 1, "expected exactly one timeseries collection after create", {colls});
    const tsOptions = colls[0].options.timeseries;
    if (fixedBucketingStrongAsserts) {
        assert.eq(tsOptions.fixedBucketing, true, "expected fixedBucketing: true after creation", {
            tsOptions,
        });
    } else {
        // `info.uuid` is present for viewless collections and absent for legacy (view) namespaces.
        // Both cases are observable in FCV upgrade/downgrade suites.
        const isLegacy = colls[0].info.uuid === undefined;
        if (isLegacy) {
            // Legacy timeseries: fixedBucketing is stripped when the collection is converted from
            // viewless on downgrade completion, so it must be absent here.
            assert(
                !tsOptions.hasOwnProperty("fixedBucketing"),
                "legacy timeseries must not carry fixedBucketing",
                {tsOptions},
            );
        } else {
            // Viewless: fixedBucketing must be present and boolean (true for fresh creation,
            // false for upgraded from legacy).
            assert.eq(
                typeof tsOptions.fixedBucketing,
                "boolean",
                "fixedBucketing must be present and boolean",
                {tsOptions},
            );
        }
    }
})();

// collMod changing bucketing params flips fixedBucketing from true to false (when the flag is on).
assert.commandWorked(
    testDB.runCommand({
        collMod: collName,
        timeseries: {bucketMaxSpanSeconds: 200, bucketRoundingSeconds: 200},
    }),
);

// Test that a valid collMod changing bucketing params always succeeds and that the resulting
// fixedBucketing value is false (viewless timeseries) or absent (legacy timeseries).
(function assertFixedBucketingAfterCollMod() {
    const colls = assert.commandWorked(
        testDB.runCommand({listCollections: 1, filter: {type: "timeseries", name: collName}}),
    ).cursor.firstBatch;
    assert.eq(colls.length, 1, "expected exactly one timeseries collection after collMod", {colls});
    const tsOptions = colls[0].options.timeseries;
    const isLegacy = colls[0].info.uuid === undefined;
    if (isLegacy) {
        assert(
            !tsOptions.hasOwnProperty("fixedBucketing"),
            "legacy timeseries must not carry fixedBucketing",
            {tsOptions},
        );
    } else {
        // Viewless: collMod must have set fixedBucketing to false.
        assert.eq(
            tsOptions.fixedBucketing,
            false,
            "viewless timeseries always have fixedBucketing set to false after collMod that changes params",
            {tsOptions},
        );
    }
})();
