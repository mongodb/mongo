import {
    checkSbeStatus,
    kFeatureFlagSbeFullEnabled,
    kSbeDisabled,
    kSbeFullyEnabled,
    kSbeRestricted
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

    // We may have different output files for different SBE configurations. If that is the case, we
    // need to pick the correct directory for the curent configuration.
    if (typeof db !== 'undefined') {
        let sbeStatus = checkSbeStatus(db);
        if (fileExists(relativePathToExpectedOutput + "/" + sbeStatus + "/" + outputName)) {
            relativePathToExpectedOutput += "/" + sbeStatus;
        }

    } else {
        // If we don't have a database available, we can only look at the TestData to see what
        // parameters resmoke was given.
        let sbeStatus;
        const frameworkControl = TestData.setParameters.internalQueryFrameworkControl
            ? TestData.setParameters.internalQueryFrameworkControl
            : "trySbeRestricted";
        if (frameworkControl == "forceClassicEngine") {
            // Always overrides anything else.
            sbeStatus = kSbeDisabled;
        } else if (TestData.setParameters.featureFlagSbeFull &&
                   TestData.setParameters.featureFlagSbeFull == "true") {
            // Otherwise, if this feature flag is enabled, we ignore the query knob.
            sbeStatus = kFeatureFlagSbeFullEnabled;
        } else if (frameworkControl === "trySbeEngine") {
            sbeStatus = kSbeFullyEnabled;
        } else {
            // If we're here, we must be using 'trySbeRestricted'.
            assert.eq(frameworkControl, "trySbeRestricted");
            sbeStatus = kSbeRestricted;
        }

        if (fileExists(relativePathToExpectedOutput + "/" + sbeStatus + "/" + outputName)) {
            relativePathToExpectedOutput += "/" + sbeStatus;
        }
    }

    _openGoldenData(outputName, {relativePath: relativePathToExpectedOutput});
}
