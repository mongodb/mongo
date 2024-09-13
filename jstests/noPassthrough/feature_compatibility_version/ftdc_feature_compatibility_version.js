/**
 * This test is to make sure that the 'featureCompatibilityVersionMajor,
 * featureCompatibilityVersionMinor, featureCompatibilityVersionTransitioning' metrics are in the
 * serverStatus metrics in FTDC data.
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {verifyGetDiagnosticData} from "jstests/libs/ftdc.js";

let conn = MongoRunner.runMongod();
let adminDb = conn.getDB('admin');

// Verify 'featureCompatibilityVersion' representation is in serverStatus metrics.
let ftdcData = verifyGetDiagnosticData(adminDb);
let fcvMajorMinor = latestFCV.split('.');

assert(ftdcData["serverStatus"].hasOwnProperty("featureCompatibilityVersion"),
       "does not have 'serverStatus.featureCompatibilityVersion' in '" + tojson(ftdcData) + "'");

let fcvMetrics = ftdcData["serverStatus"]["featureCompatibilityVersion"];

assert(
    fcvMetrics.hasOwnProperty("major"),
    "does not have 'serverStatus.featureCompatibilityVersion.major' in '" + tojson(ftdcData) + "'");

assert.eq(fcvMetrics["major"],
          fcvMajorMinor[0],
          fcvMetrics["major"] + " does not match expected FCV major number " + fcvMajorMinor[0]);

assert(
    fcvMetrics.hasOwnProperty("minor"),
    "does not have 'serverStatus.featureCompatibilityVersion.minor' in '" + tojson(ftdcData) + "'");

assert.eq(fcvMetrics["minor"],
          fcvMajorMinor[1],
          fcvMetrics["minor"] + " does not match expected FCV minor number " + fcvMajorMinor[1]);

assert(fcvMetrics.hasOwnProperty("transitioning"),
       "does not have 'serverStatus.featureCompatibilityVersion.transitioning' in '" +
           tojson(ftdcData) + "'");

assert.eq(fcvMetrics["transitioning"], 0, "expected transitioning to be 0 in " + tojson(ftdcData));

// Set hangpoint then do downgrade and make sure we see transitioning.
const hangWhileDowngradingFp = configureFailPoint(adminDb, "hangBeforeTransitioningToDowngraded");
let lastLTSFCVMajorMinor = lastLTSFCV.split('.');

const downgradeThread = startParallelShell(function() {
    assert.commandWorked(
        db.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));
}, conn.port);

assert.soon(function() {
    ftdcData = verifyGetDiagnosticData(adminDb);
    fcvMetrics = ftdcData["serverStatus"]["featureCompatibilityVersion"];
    return fcvMetrics["major"] == lastLTSFCVMajorMinor[0] &&
        fcvMetrics["minor"] == lastLTSFCVMajorMinor[1] && fcvMetrics["transitioning"] == -1;
});

hangWhileDowngradingFp.off();
downgradeThread();

// Make sure downgrade finishes after failpoint.
assert.soon(function() {
    ftdcData = verifyGetDiagnosticData(adminDb);
    fcvMetrics = ftdcData["serverStatus"]["featureCompatibilityVersion"];
    return fcvMetrics["major"] == lastLTSFCVMajorMinor[0] &&
        fcvMetrics["minor"] == lastLTSFCVMajorMinor[1] && fcvMetrics["transitioning"] == 0;
});

// Test upgrading logs correctly.
let hangWhileUpgradingFp = configureFailPoint(adminDb, "hangWhileUpgrading");

const upgradeThread = startParallelShell(function() {
    assert.commandWorked(
        db.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));
}, conn.port);

assert.soon(function() {
    ftdcData = verifyGetDiagnosticData(adminDb);
    fcvMetrics = ftdcData["serverStatus"]["featureCompatibilityVersion"];
    return fcvMetrics["major"] == fcvMajorMinor[0] && fcvMetrics["minor"] == fcvMajorMinor[1] &&
        fcvMetrics["transitioning"] == 1;
});

hangWhileUpgradingFp.off();
upgradeThread();

// Make sure downgrade finishes after failpoint.
assert.soon(function() {
    ftdcData = verifyGetDiagnosticData(adminDb);
    fcvMetrics = ftdcData["serverStatus"]["featureCompatibilityVersion"];
    return fcvMetrics["major"] == fcvMajorMinor[0] && fcvMetrics["minor"] == fcvMajorMinor[1] &&
        fcvMetrics["transitioning"] == 0;
});

MongoRunner.stopMongod(conn);
