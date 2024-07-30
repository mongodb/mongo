import {checkSbeStatus} from "jstests/libs/sbe_util.js";

// Run any set-up necessary for a golden jstest. This function should be called from the suite
// definition, so that individual tests don't need to remember to call it.
export function beginGoldenTest(relativePathToExpectedOutput) {
    let sbeStatus = checkSbeStatus(db);

    if (fileExists(relativePathToExpectedOutput + "/" + sbeStatus + "/" + jsTestName())) {
        relativePathToExpectedOutput += "/" + sbeStatus;
    }

    _openGoldenData(jsTestName(), {relativePath: relativePathToExpectedOutput});
}
