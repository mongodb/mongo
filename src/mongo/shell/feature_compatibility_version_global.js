// Populate global variables from modules for backwards compatibility

import {
    binVersionToFCV,
    checkFCV,
    removeFCVDocument,
    runFeatureFlagMultiversionTest,
} from "jstests/libs/feature_compatibility_version.js";

/**
 * These constants represent the current "latest", "last-continuous" and "last-lts" values for the
 * featureCompatibilityVersion parameter. They should only be used for testing of upgrade-downgrade
 * scenarios that are intended to be maintained between releases.
 *
 * We cannot use `const` when declaring them because it must be possible to load() this file
 * multiple times.
 */

let fcvConstants = getFCVConstants();

globalThis.lastContinuousFCV = fcvConstants.lastContinuous;
globalThis.lastLTSFCV = fcvConstants.lastLTS;
globalThis.latestFCV = fcvConstants.latest;
// The number of versions since the last-lts version. When numVersionsSinceLastLTS = 1,
// lastContinuousFCV is equal to lastLTSFCV. This is used to calculate the expected minWireVersion
// in jstests that use the lastLTSFCV.
globalThis.numVersionsSinceLastLTS = fcvConstants.numSinceLastLTS;

globalThis.binVersionToFCV = binVersionToFCV;
globalThis.checkFCV = checkFCV;
globalThis.removeFCVDocument = removeFCVDocument;
globalThis.runFeatureFlagMultiversionTest = runFeatureFlagMultiversionTest;
