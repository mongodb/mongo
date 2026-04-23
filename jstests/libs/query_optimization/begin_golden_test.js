import {getPlanRankerMode, getAutomaticCEPlanRankingStrategy} from "jstests/libs/query/cbr_utils.js";
import {
    checkSbeStatus,
    checkJoinOptimizationStatus,
    checkSbeNonLeadingMatchEnabled,
    checkSbeEqLookupUnwindEnabled,
    checkSbeTransformStagesEnabled,
    isDeferredGetExecutorEnabled,
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
    if (testNameParts.length > 0 && testNameParts[testNameParts.length - 1] === "md" && fileExtension === "") {
        fileExtension = ".md";
        outputName = testNameParts.slice(0, -1).join("_");
    }

    outputName += fileExtension;

    // We may have different output files for different SBE or CBR configurations. If that is the
    // case, we need to pick the correct directory for the curent configuration.
    const sbeStatus = checkSbeStatus(typeof db === "undefined" ? null : db);
    const planRankerMode = getPlanRankerMode(typeof db === "undefined" ? null : db);
    const autoPlanRankingStrategy = getAutomaticCEPlanRankingStrategy(typeof db === "undefined" ? null : db);
    const joinOptimizationStatus = checkJoinOptimizationStatus(typeof db === "undefined" ? null : db);
    const sbeNonLeadingMatchEnabled = checkSbeNonLeadingMatchEnabled(typeof db === "undefined" ? null : db);
    const sbeEqLookupUnwindEnabled = checkSbeEqLookupUnwindEnabled(typeof db === "undefined" ? null : db);
    const sbeTransformStagesEnabled = checkSbeTransformStagesEnabled(typeof db === "undefined" ? null : db);
    const deferredGetExecutorStatus = isDeferredGetExecutorEnabled(typeof db === "undefined" ? null : db);
    const sbeIndividualFeaturesEnabled =
        sbeNonLeadingMatchEnabled && sbeEqLookupUnwindEnabled && sbeTransformStagesEnabled;

    const sbeIndividualFeaturesExpectedExists = fileExists(
        relativePathToExpectedOutput + "/sbeIndividualFeatures/" + outputName,
    );
    const sbeExpectedExists = fileExists(relativePathToExpectedOutput + "/" + sbeStatus + "/" + outputName);

    const outputDirPlanRanking =
        planRankerMode != "automaticCE"
            ? relativePathToExpectedOutput + "/" + planRankerMode
            : relativePathToExpectedOutput + "/" + planRankerMode + "/" + autoPlanRankingStrategy;
    const planRankerModeExpectedExists = fileExists(outputDirPlanRanking + "/" + outputName);

    const deferredGetExecutorExpectedExists = fileExists(
        relativePathToExpectedOutput + "/deferredGetExecutor/" + outputName,
    );

    const joinOptimizationExpectedExists = fileExists(
        relativePathToExpectedOutput + "/internalEnableJoinOptimization/" + outputName,
    );

    if (joinOptimizationStatus && joinOptimizationExpectedExists) {
        relativePathToExpectedOutput += "/internalEnableJoinOptimization";
    } else if (deferredGetExecutorStatus && deferredGetExecutorExpectedExists) {
        relativePathToExpectedOutput += "/deferredGetExecutor";
    } else if (sbeIndividualFeaturesEnabled && sbeIndividualFeaturesExpectedExists) {
        relativePathToExpectedOutput += "/sbeIndividualFeatures";
    } else if (sbeExpectedExists && planRankerModeExpectedExists) {
        // Both SBE and CBR expected outputs exist, bail.
        assert.fail(
            "Both SBE and CBR expected outputs exist for " + outputName + ", cannot determine which one to use. ",
        );
    } else if (sbeExpectedExists) {
        relativePathToExpectedOutput += "/" + sbeStatus;
    } else if (planRankerModeExpectedExists) {
        relativePathToExpectedOutput = outputDirPlanRanking;
    }

    _openGoldenData(outputName, {relativePath: relativePathToExpectedOutput});
}
