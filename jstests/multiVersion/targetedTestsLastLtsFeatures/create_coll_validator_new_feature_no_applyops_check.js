/**
 * Creating a collection with a validator with expressions that are enabled on 8.1+ can interleave
 * with FCV downgrade, such that the primary checks the query feature flag on FCV 8.1+ and
 * successfully creates the collection. Then an oplog entry is committed with the new query feature
 * in the validator. The secondary will try to apply the oplog entry on the downgraded FCV and fail
 * to create the collection since the validator fails to parse.
 *
 * The secondary must not check feature flags when applying oplog entries and parsing expressions.
 */

import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {assertCommandWorkedInParallelShell} from "jstests/libs/parallel_shell_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

// TODO SERVER-104457 Add a permanent test for this scenario.

// This test depends on a specific lastLTS version and should only run when 8.0 is lastLTS.
if (lastLTSFCV !== '8.0') {
    jsTest.log.info("Skipping test since lastLTS is greater than 8.0", lastLTSFCV);
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

// Any query feature that is disabled in lastLTS but enabled in latestFCV suffices. We will use
// $sigmoid, $convert numeric, and the 'score' metadata field.

// $sigmoid checks if the feature flag is enabled with the REGISTER_X_WITH_FEATURE_FLAG macro.
runTest({$expr: {$eq: ["$stuff", {$sigmoid: 1337}]}} /* validatorSpec */);

// $convert numeric checks if a feature flag is enabled during parsing.
runTest({
    $expr: {$convert: {to: "binData", input: "$DoubleInput", byteOrder: "big"}}
} /* validatorSpec */);

// $meta checks if a feature flag is enabled during parsing.
runTest({$expr: {$meta: "score"}} /* validatorSpec */);

rst.stopSet();
