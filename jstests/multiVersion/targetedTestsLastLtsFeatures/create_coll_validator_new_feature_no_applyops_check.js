/**
 * Creating a collection with a validator with expressions that are enabled on 8.0+ can interleave
 * with FCV downgrade, such that the primary checks the query feature flag on FCV 8.0+ and
 * successfully creates the collection. Then an oplog entry is committed with the new query feature
 * in the validator. The secondary will try to apply the oplog entry on the downgraded FCV and fail
 * to create the collection since the validator fails to parse.
 *
 * The secondary must not check feature flags when applying oplog entries and parsing expressions.
 */

import "jstests/multiVersion/libs/multi_rs.js";
import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {assertCommandWorkedInParallelShell} from "jstests/libs/parallel_shell_helpers.js";

// This test depends on a specific lastLTS version and should only run when 7.0 is lastLTS.
if (lastLTSFCV !== '7.0') {
    jsTest.log.info("Skipping test since lastLTS is greater than 7.0", lastLTSFCV);
    quit();
}

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();
const primary = rst.getPrimary();
const collName = jsTestName();

function runTest(validatorSpec) {
    // Start creating a collection that has a validator with new query features.
    // Hang it after the validator has been parsed, but before the create entry has been put in the
    // oplog.
    const fpCreate = configureFailPoint(primary, 'hangAfterParsingValidator');

    const awaitCreate = assertCommandWorkedInParallelShell(
        primary, primary.getDB("test"), {create: collName, validator: validatorSpec});
    fpCreate.wait();

    // Start downgrading to last LTS. Wait until the transition has started, at which point the
    // feature flags controlling the query features will have been disabled. We need to do this in
    // background because the downgrade will hang on the global lock barrier, which happens *after*
    // transitioning.
    const awaitSetFCV = assertCommandWorkedInParallelShell(
        primary,
        primary.getDB("admin"),
        {setFeatureCompatibilityVersion: lastLTSFCV, confirm: true});
    assert.soon(() => {
        const fcvDoc = assert
                           .commandWorked(primary.adminCommand(
                               {getParameter: 1, featureCompatibilityVersion: 1}))
                           .featureCompatibilityVersion;
        return fcvDoc.version == lastLTSFCV;
    });

    // Now, let the collection creation continue and produce the create oplog entry.
    // The secondaries are already on the last LTS FCV, so they should not re-check feature flags
    // when applying the oplog entry, as they would be using a different FCV than the primary.
    fpCreate.off();
    awaitCreate();

    awaitSetFCV();

    // Reset for the next test.
    assertDropCollection(primary.getDB("test"), collName);
    assert.commandWorked(
        primary.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));
}

// '$toUUID' checks if the feature flag is enabled with the REGISTER_X_WITH_FEATURE_FLAG macro.
runTest({$expr: {$eq: ["$stuff", {$toUUID: "$x"}]}} /* validatorSpec */);

// The 'format' argument to $convert is checked during parsing and is not allowed on FCVs below 8.0.
runTest({
    $expr: {$convert: {input: "$stuff", to: {type: "binData", subtype: 0}, format: "base64"}}
} /* validatorSpec */);

// TODO BACKPORT-25868 enable test
// $meta checks if a feature flag is enabled during parsing.
// runTest({$expr: {$meta: "score"}} /* validatorSpec */);

rst.stopSet();
