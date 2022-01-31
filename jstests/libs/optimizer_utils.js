/*
 * Utility for checking if the query optimizer is enabled.
 */
function checkCascadesOptimizerEnabled(theDB) {
    const param = theDB.adminCommand({getParameter: 1, featureFlagCommonQueryFramework: 1});
    return param.hasOwnProperty("featureFlagCommonQueryFramework") &&
        param.featureFlagCommonQueryFramework.value;
}
