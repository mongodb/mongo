import {checkSbeStatus} from "jstests/libs/query/sbe_util.js";

// Run any set-up necessary for a golden jstest. This function should be called from the suite
// definition, so that individual tests don't need to remember to call it.
//
// In case the test name ends in "_md", the golden data will be outputted to a markdown file.
// However, if an explicit fileExtension is specified, it will always be used instead.
export function beginGoldenTest(relativePathToExpectedOutput, fileExtension = "") {
    // Skip checking SBE status if there is no `db` object when nodb:"" is used.
    if (typeof db !== 'undefined') {
        let sbeStatus = checkSbeStatus(db);

        if (fileExists(relativePathToExpectedOutput + "/" + sbeStatus + "/" + jsTestName())) {
            relativePathToExpectedOutput += "/" + sbeStatus;
        }
    }

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

    _openGoldenData(outputName + fileExtension, {relativePath: relativePathToExpectedOutput});
}
