// Populate global variables from modules for backwards compatibility

import {
    binVersionToFCV,
    checkFCV,
    isFCVEqual,
    lastContinuousFCV,
    lastLTSFCV,
    latestFCV,
    numVersionsSinceLastLTS,
    removeFCVDocument,
    runFeatureFlagMultiversionTest
} from "src/mongo/shell/feature_compatibility_version.js";

globalThis.binVersionToFCV = binVersionToFCV;
globalThis.checkFCV = checkFCV;
globalThis.isFCVEqual = isFCVEqual;
globalThis.lastContinuousFCV = lastContinuousFCV;
globalThis.lastLTSFCV = lastLTSFCV;
globalThis.latestFCV = latestFCV;
globalThis.numVersionsSinceLastLTS = numVersionsSinceLastLTS;
globalThis.removeFCVDocument = removeFCVDocument;
globalThis.runFeatureFlagMultiversionTest = runFeatureFlagMultiversionTest;
