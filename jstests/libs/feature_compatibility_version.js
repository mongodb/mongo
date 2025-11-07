/**
 * Utility function for working with cluster Feature Compatibilty Version.
 */

function getFCVDocument(conn) {
    let adminDB = typeof conn.getDB === "function" ? conn.getDB("admin") : conn.getSiblingDB("admin");
    return adminDB["system.version"].findOne({_id: "featureCompatibilityVersion"});
}

export function getCurrentFCV(conn) {
    const FCVDoc = getFCVDocument(conn);
    assert(FCVDoc, "Failed to retrieve FCV document");
    return FCVDoc.version;
}

/**
 * Returns true if we are running in a test suite with stable FCV.
 */
export function isStableFCVSuite() {
    return !TestData.isRunningFCVUpgradeDowngradeSuite;
}

export function isFCVgt(conn, targetVersion) {
    const lowestFCV = isStableFCVSuite() ? getCurrentFCV(conn) : lastLTSFCV;
    return MongoRunner.compareBinVersions(lowestFCV, targetVersion) > 0;
}

export function isFCVgte(conn, targetVersion) {
    const lowestFCV = isStableFCVSuite() ? getCurrentFCV(conn) : lastLTSFCV;
    return MongoRunner.compareBinVersions(lowestFCV, targetVersion) >= 0;
}

export function isFCVlt(conn, targetVersion) {
    const highestFCV = isStableFCVSuite() ? getCurrentFCV(conn) : latestFCV;
    return MongoRunner.compareBinVersions(highestFCV, targetVersion) < 0;
}

export function isFCVlte(conn, targetVersion) {
    const highestFCV = isStableFCVSuite() ? getCurrentFCV(conn) : latestFCV;
    return MongoRunner.compareBinVersions(highestFCV, targetVersion) <= 0;
}

export function isFCVeq(conn, targetVersion) {
    assert(isStableFCVSuite(), "Can't use `isFCVeq` function in suites that perform backround FCV transitions.");
    const currentFCV = getCurrentFCV(conn);
    return MongoRunner.compareBinVersions(currentFCV, targetVersion) == 0;
}
