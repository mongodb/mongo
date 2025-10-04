// Contains helpers for checking the featureCompatibilityVersion and constants for the current
// featureCompatibilityVersion values.

/**
 * These constants represent the current "latest", "last-continuous" and "last-lts" values for the
 * featureCompatibilityVersion parameter. They should only be used for testing of upgrade-downgrade
 * scenarios that are intended to be maintained between releases.
 *
 * We cannot use `const` when declaring them because it must be possible to load() this file
 * multiple times.
 */

let fcvConstants = getFCVConstants();

let latestFCV = fcvConstants.latest;
let lastContinuousFCV = fcvConstants.lastContinuous;
let lastLTSFCV = fcvConstants.lastLTS;
// The number of versions since the last-lts version. When numVersionsSinceLastLTS = 1,
// lastContinuousFCV is equal to lastLTSFCV. This is used to calculate the expected minWireVersion
// in jstests that use the lastLTSFCV.
let numVersionsSinceLastLTS = fcvConstants.numSinceLastLTS;

/**
 * Returns the FCV associated with a binary version.
 * eg. An input of 'last-lts' will return lastLTSFCV.
 */
function binVersionToFCV(binVersion) {
    if (binVersion === "latest") {
        return latestFCV;
    }
    return binVersion === "last-lts" ? lastLTSFCV : lastContinuousFCV;
}

/**
 * Checks the featureCompatibilityVersion document and server parameter. The
 * featureCompatibilityVersion document is of the form {_id: "featureCompatibilityVersion", version:
 * <required>, targetVersion: <optional>, previousVersion: <optional>}. The getParameter result is
 * of the form {featureCompatibilityVersion: {version: <required>, targetVersion: <optional>,
 * previousVersion: <optional>}, ok: 1}.
 */
function checkFCV(adminDB, version, targetVersion, isCleaningServerMetadata) {
    // When both version and targetVersion are equal to lastContinuousFCV or lastLTSFCV, downgrade
    // is in progress. This tests that previousVersion is always equal to latestFCV in downgrading
    // states or undefined otherwise.
    const isDowngrading =
        (version === lastLTSFCV && targetVersion === lastLTSFCV) ||
        (version === lastContinuousFCV && targetVersion === lastContinuousFCV);

    const isMongod = !adminDB.getMongo().isMongos();
    if (isMongod) {
        let res = adminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
        assert.commandWorked(res);
        assert.eq(
            res.featureCompatibilityVersion.version,
            version,
            "FCV server parameter 'version' field does not match: " + tojson(res),
        );
        assert.eq(
            res.featureCompatibilityVersion.targetVersion,
            targetVersion,
            "FCV server parameter 'targetVersion' field does not match: " + tojson(res),
        );
        if (isDowngrading) {
            assert.eq(
                res.featureCompatibilityVersion.previousVersion,
                latestFCV,
                "FCV server parameter 'previousVersion' field does not match: " + tojson(res),
            );
        } else {
            assert.eq(
                res.featureCompatibilityVersion.previousVersion,
                undefined,
                "FCV server parameter 'previousVersion' field does not match: " + tojson(res),
            );
        }
    }

    // This query specifies an explicit readConcern because some FCV tests pass a connection that
    // has manually run isMaster with internalClient, and mongod expects internalClients (ie. other
    // cluster members) to include read/write concern (on commands that accept read/write concern).
    let doc = adminDB.system.version.find({_id: "featureCompatibilityVersion"}).limit(1).readConcern("local").next();
    assert.eq(doc.version, version, "FCV document 'version' field does not match: " + tojson(doc));
    assert.eq(doc.targetVersion, targetVersion, "FCV document 'targetVersion' field does not match: " + tojson(doc));
    if (isDowngrading) {
        assert.eq(
            doc.previousVersion,
            latestFCV,
            "FCV document 'previousVersion' field does not match: " + tojson(doc),
        );
    } else {
        assert.eq(
            doc.previousVersion,
            undefined,
            "FCV document 'previousVersion' field does not match: " + tojson(doc),
        );
    }
    if (isCleaningServerMetadata) {
        assert.eq(
            doc.isCleaningServerMetadata,
            true,
            "FCV document 'isCleaningServerMetadata' field should be true: " + tojson(doc),
        );
    } else {
        assert.eq(
            doc.isCleaningServerMetadata,
            undefined,
            "FCV document 'isCleaningServerMetadata' field should not exist, but did: " + tojson(doc),
        );
    }
}

/**
 * Returns true if checkFCV runs successfully.
 */
function isFCVEqual(adminDB, version, targetVersion) {
    try {
        checkFCV(adminDB, version, targetVersion);
    } catch (e) {
        jsTestLog("checkFCV failed with error: " + tojson(e));
        return false;
    }
    return true;
}

/**
 * Since SERVER-29453 disallowed removal of the FCV document, we need to do this hack to remove it.
 */
function removeFCVDocument(adminDB) {
    let res = adminDB.runCommand({listCollections: 1, filter: {name: "system.version"}});
    assert.commandWorked(res, "failed to list collections");
    let originalUUID = res.cursor.firstBatch[0].info.uuid;
    let newUUID = UUID();

    // Create new collection with no FCV document, and then delete the
    // original collection.
    let createNewAdminSystemVersionCollection = {op: "c", ns: "admin.$cmd", ui: newUUID, o: {create: "system.version"}};
    let dropOriginalAdminSystemVersionCollection = {
        op: "c",
        ns: "admin.$cmd",
        ui: originalUUID,
        o: {drop: "admin.tmp_system_version"},
    };
    assert.commandWorked(
        adminDB.runCommand({
            applyOps: [createNewAdminSystemVersionCollection, dropOriginalAdminSystemVersionCollection],
        }),
    );

    res = adminDB.runCommand({listCollections: 1, filter: {name: "system.version"}});
    assert.commandWorked(res, "failed to list collections");
    assert.eq(newUUID, res.cursor.firstBatch[0].info.uuid);
}

/**
 * Runs 'testFunc' with the last-lts FCV as an argument. If 'featureFlag' has a release version
 * equal to the latest FCV, 'testFunc' will also be run a second time but with last-continuous FCV
 * as the argument.
 *
 * If featureFlag does not exist in the server, throw an error.
 * If featureFlag is not enabled, return without running 'testFunct'.
 *
 * 'testFunc' is expected to be a function that accepts a valid downgrade FCV as input.
 */
function runFeatureFlagMultiversionTest(featureFlag, testFunc) {
    jsTestLog("Running standalone to gather parameter info about featureFlag: " + featureFlag);
    // Spin up a standalone to check the release version of 'featureFlag'.
    let standalone = MongoRunner.runMongod();
    let adminDB = standalone.getDB("admin");
    let res;
    try {
        res = assert.commandWorked(
            adminDB.runCommand({getParameter: 1, [featureFlag]: 1}),
            "Failed to call getParameter on feature flag: " + featureFlag,
        );
    } finally {
        MongoRunner.stopMongod(standalone);
    }

    if (res && !res[featureFlag]["value"]) {
        jsTestLog("Feature flag: " + featureFlag + " is not enabled. Skipping test.");
        return;
    }

    jsTestLog("Running testFunc with last-lts FCV.");
    testFunc(lastLTSFCV);
    if (
        res &&
        res[featureFlag].hasOwnProperty("version") &&
        MongoRunner.compareBinVersions(res[featureFlag]["version"].toString(), "latest") === 0
    ) {
        // The feature associated with 'featureFlag' will be released in the latest FCV. We should
        // also run upgrade/downgrade behavior against the last-continuous FCV.
        jsTestLog("Running testFunc with last-continuous FCV.");
        testFunc(lastContinuousFCV);
    }
}

export {
    binVersionToFCV,
    checkFCV,
    isFCVEqual,
    lastContinuousFCV,
    lastLTSFCV,
    latestFCV,
    numVersionsSinceLastLTS,
    removeFCVDocument,
    runFeatureFlagMultiversionTest,
};
