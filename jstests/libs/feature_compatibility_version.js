/**
 * Utility function for working with cluster Feature Compatibilty Version.
 */
import {isMongos} from "jstests/concurrency/fsm_workload_helpers/server_types.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

function getAdminDB(conn) {
    return typeof conn.getDB === "function" ? conn.getDB("admin") : conn.getSiblingDB("admin");
}

function getFCVDocument(conn) {
    return getAdminDB(conn)["system.version"].findOne({_id: "featureCompatibilityVersion"});
}

export function getCurrentFCV(conn) {
    const FCVDoc = getFCVDocument(conn);
    assert(FCVDoc, "Failed to retrieve FCV document");
    return FCVDoc.version;
}

/**
 * Returns the FCV used for the last stable binary version.
 *
 * TODO (SERVER-133737): Return the distinct last stable FCV once it is exposed to JavaScript tests.
 */
export function getLastStableFCV() {
    return lastContinuousFCV;
}

function getMultiversionBinVersion() {
    return (
        TestData.multiversionBinVersion ||
        TestData.useRandomBinVersionsWithinReplicaSet ||
        (typeof jsTest !== "undefined" && jsTest.options().useRandomBinVersionsWithinReplicaSet)
    );
}

function getMultiversionFCV() {
    const multiversionBinVersion = getMultiversionBinVersion();

    if (multiversionBinVersion === "last-lts") {
        return lastLTSFCV;
    }
    if (multiversionBinVersion === "last-continuous") {
        return lastContinuousFCV;
    }
    return undefined;
}

/**
 * Returns a direct mongod connection when needed for server parameters unavailable through mongos.
 * FCV is read from the fixture's main connection because setFCV and addShard replicate it cluster-wide.
 */
function getMongodConn(conn) {
    if (!isMongos(conn.getDB("admin"))) {
        return conn;
    }

    // Use the config server and retry connection establishment during stepdown/terminate
    // passthroughs. Existing runCommand overrides retry subsequent network errors.
    const connString = FixtureHelpers.getConfigServerConnString(conn.getDB("admin"));
    return retryOnRetryableError(() => {
        return new Mongo(connString, undefined /* encryptedDBClientCallback */, {gRPC: false});
    });
}

function isDisaggregatedStorageEnabled(conn) {
    const response = assert.commandWorkedOrFailedWithCode(
        getMongodConn(conn).getDB("admin").runCommand({
            getParameter: 1,
            disaggregatedStorageEnabled: 1,
        }),
        ErrorCodes.InvalidOptions,
    );
    return response.ok === 1 && response.disaggregatedStorageEnabled === true;
}

function shouldSkipSuiteFCVCheck() {
    // FCV upgrade/downgrade suites intentionally change FCV while tests are running.
    if (TestData.isRunningFCVUpgradeDowngradeSuite) {
        return true;
    }

    // The shell does not expose the last-patch FCV. The resmoke fixture builder sets it using the
    // multiversion service, so it cannot be checked here without duplicating that implementation.
    return getMultiversionBinVersion() === "last-patch";
}

/**
 * Returns the FCV expected by the current suite.
 */
function getExpectedSuiteFCV(conn) {
    const multiversionFCV = getMultiversionFCV();
    if (multiversionFCV !== undefined) {
        return multiversionFCV;
    }
    assert(!TestData.mixedBinVersions, "Cannot determine expected FCV for a multiversion suite", {
        multiversionBinVersion: getMultiversionBinVersion(),
        mixedBinVersions: TestData.mixedBinVersions,
    });

    if (isDisaggregatedStorageEnabled(conn)) {
        return TestData.runAllFeatureFlagTests ? latestFCV : getLastStableFCV();
    }
    return latestFCV;
}

/**
 * Asserts that the fixture has the FCV expected by the current suite.
 *
 * 'conn' must be a Mongo connection to the fixture (e.g. db.getMongo(), rst.getPrimary(), st.s).
 * FCV changes are replicated across the fixture, so it is sufficient to read the FCV from the
 * fixture's main connection.
 *
 * Returns false for suites that change FCV in the background or use last-patch binaries.
 */
export function assertCurrentFCVMatchesSuite(conn) {
    if (shouldSkipSuiteFCVCheck()) {
        jsTest.log.info("Skipping FCV suite self-test", {
            isRunningFCVUpgradeDowngradeSuite: Boolean(TestData.isRunningFCVUpgradeDowngradeSuite),
            multiversionBinVersion: getMultiversionBinVersion(),
        });
        return false;
    }

    const currentFCV = getCurrentFCV(conn);
    const expectedFCV = getExpectedSuiteFCV(conn);
    assert.eq(currentFCV, expectedFCV, "The fixture FCV does not match the suite expectation", {
        currentFCV,
        expectedFCV,
        host: conn.host,
    });
    return true;
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
    assert(
        isStableFCVSuite(),
        "Can't use `isFCVeq` function in suites that perform backround FCV transitions.",
    );
    const currentFCV = getCurrentFCV(conn);
    return MongoRunner.compareBinVersions(currentFCV, targetVersion) == 0;
}
