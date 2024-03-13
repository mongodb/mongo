/**
 * Helper function to determine if the dots and dollars feature flag is enabled.
 * For the dots and dollars feature to be enabled we need the following:
 * 1. The feature flag must be enabled.
 * 2. The FCV must be 5.0.
 *
 * @returns {boolean} true if the dots and dollars feature flag is enabled and the FCV is 5.0.
 */

function readFCV(db) {
    const adminDB = db.getSiblingDB('admin');
    const isMongod = !adminDB.getMongo().isMongos();
    if (isMongod) {
        const res = adminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
        assert.commandWorked(res);
        return res;
    }

    const doc = adminDB.system.version.find({_id: "featureCompatibilityVersion"}).limit(1).next();
    return {"featureCompatibilityVersion": doc};
}

function isDotsAndDollarsEnabled(overrideDb) {
    const dbToUse = overrideDb || db;
    const flagParam = dbToUse.adminCommand({getParameter: 1, featureFlagDotsAndDollars: 1});
    const fcvParam = readFCV(dbToUse);
    const result = flagParam.featureFlagDotsAndDollars &&
        flagParam.featureFlagDotsAndDollars.value && fcvParam.featureCompatibilityVersion &&
        fcvParam.featureCompatibilityVersion.version == "5.0";
    jsTestLog(`isDotsAndDollarseEnabled: ${result}.\nflagParam: ${tojson(flagParam)},\nfcvParam: ${
        tojson(fcvParam)}`);
    return result;
}
