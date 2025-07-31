import {
    getPlanRankerMode,
} from "jstests/libs/query/cbr_utils.js";
import {
    checkSbeStatus,
} from "jstests/libs/query/sbe_util.js";

// Run any set-up necessary for a golden jstest. This function should be called from the suite
// definition, so that individual tests don't need to remember to call it.
//
// In case the test name ends in "_md", the golden data will be outputted to a markdown file.
// However, if an explicit fileExtension is specified, it will always be used instead.
export function beginGoldenTest(relativePathToExpectedOutput, fileExtension = "") {
    let outputName = jsTestName();
    const testNameParts = jsTestName().split("_");

    // If the test name ends in "_md" and no explicit file extension is specified, then remove the
    // "_md" part and use it as the file extension.
    // TODO SERVER-92693: Use only the file extension.
    if (testNameParts.length > 0 && testNameParts[testNameParts.length - 1] === "md" &&
        fileExtension === "") {
        fileExtension = ".md";
        outputName = testNameParts.slice(0, -1).join("_");
    }

    outputName += fileExtension;

    // We may have different output files for different SBE or CBR configurations. If that is the
    // case, we need to pick the correct directory for the curent configuration.
    const sbeStatus = checkSbeStatus(typeof db === "undefined" ? null : db);
    const planRankerMode = getPlanRankerMode(typeof db === "undefined" ? null : db);

    const sbeExpectedExists =
        fileExists(relativePathToExpectedOutput + "/" + sbeStatus + "/" + outputName);
    const planRankerModeExpectedExits =
        fileExists(relativePathToExpectedOutput + "/" + planRankerMode + "/" + outputName);

    if (sbeExpectedExists && planRankerModeExpectedExits) {
        // Both SBE and CBR expected outputs exist, bail.
        assert.fail("Both SBE and CBR expected outputs exist for " + outputName +
                    ", cannot determine which one to use. ");
    } else if (sbeExpectedExists) {
        relativePathToExpectedOutput += "/" + sbeStatus;
    } else if (planRankerModeExpectedExits) {
        relativePathToExpectedOutput += "/" + planRankerMode;
    }

    _openGoldenData(outputName, {relativePath: relativePathToExpectedOutput});
}
