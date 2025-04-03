/**
 * Tests that async FTDC parameters behave properly.
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {verifyGetDiagnosticData} from "jstests/libs/ftdc.js";

const kDefaultPeriod = 1000;
const kDefaultTimeout = 166;
const kDefaultMinThreads = 1;
const kDefaultMaxThreads = 1;

let conn = MongoRunner.runMongod({
    setParameter: {
        featureFlagGaplessFTDC: true,
        diagnosticDataCollectionPeriodMillis: kDefaultPeriod,
        diagnosticDataCollectionSampleTimeoutMillis: kDefaultTimeout,
        diagnosticDataCollectionMinThreads: kDefaultMinThreads,
        diagnosticDataCollectionMaxThreads: kDefaultMaxThreads,
    }
});
let adminDb = conn.getDB('admin');

function setParameter(parameter, value) {
    let command = {setParameter: 1};
    command[parameter] = value;
    assert.commandWorked(adminDb.runCommand(command));
}

function resetParameters() {
    setParameter("diagnosticDataCollectionPeriodMillis", kDefaultPeriod);
    setParameter("diagnosticDataCollectionSampleTimeoutMillis", kDefaultTimeout);
    setParameter("diagnosticDataCollectionMinThreads", kDefaultMinThreads);
    setParameter("diagnosticDataCollectionMaxThreads", kDefaultMaxThreads);
}

function getNextSample() {
    let originalSample = assert.commandWorked(adminDb.runCommand("getDiagnosticData")).data;
    let currData;
    assert.soon(() => {
        currData = assert.commandWorked(adminDb.runCommand("getDiagnosticData")).data;
        return currData.start > originalSample.start;
    }, "Timeout waiting for next FTDC sample", 30 * 1000);

    return currData;
}

function configureFailPointAndWaitUntilHit(delay) {
    let fp =
        configureFailPoint(conn, "injectFTDCServerStatusCollectionDelay", {sleepTimeMillis: delay});
    fp.waitWithTimeout(2000);

    return fp;
}

function waitUntilNormalOperation() {
    // Since we are injecting delays into server status using the
    // injectFTDCServerStatusCollectionDelay failpoint, we define "normal" ftdc operation as having
    // data for serverStatus.
    assert.soon(() => {
        let data = assert.commandWorked(adminDb.runCommand("getDiagnosticData")).data;
        return data.hasOwnProperty("serverStatus");
    }, "Timeout waiting for FTDC to operate normally.", 90 * 1000);
}

function testSampleTimeout() {
    jsTestLog("Running sample timeout test");

    let invalidTimeoutCmd = {
        setParameter: 1,
        diagnosticDataCollectionSampleTimeoutMillis: kDefaultPeriod
    };
    assert.commandFailedWithCode(adminDb.runCommand(invalidTimeoutCmd), ErrorCodes.InvalidOptions);

    let fp = configureFailPointAndWaitUntilHit(200);
    getNextSample();

    const mongoOutput = rawMongoProgramOutput(".*");
    let logMatcher = /Collection timed out on collector/;
    assert(logMatcher.test(mongoOutput));

    setParameter("diagnosticDataCollectionSampleTimeoutMillis", 250);

    clearRawMongoProgramOutput();
    getNextSample();

    const moreMongoOutput = rawMongoProgramOutput(".*");
    assert(!logMatcher.test(moreMongoOutput));

    fp.off();
}

function testMinThreads() {
    jsTestLog("Running min threads test");

    assert.commandFailedWithCode(
        adminDb.runCommand({setParameter: 1, diagnosticDataCollectionMinThreads: 2}),
        ErrorCodes.BadValue);

    setParameter("diagnosticDataCollectionMaxThreads", 4);
    setParameter("diagnosticDataCollectionMinThreads", 2);
    let fp = configureFailPointAndWaitUntilHit(5000);

    let data = getNextSample();
    assert(data.hasOwnProperty("transportLayerStats"));
    assert(!data.hasOwnProperty("serverStatus"));

    fp.off();
}

function testMaxThreads() {
    jsTestLog("Running max threads test");

    assert.commandFailedWithCode(
        adminDb.runCommand({setParameter: 1, diagnosticDataCollectionMaxThreads: 0}),
        ErrorCodes.BadValue);

    const newPeriod = 500;
    setParameter("diagnosticDataCollectionPeriodMillis", newPeriod);
    let fp = configureFailPointAndWaitUntilHit(5000);

    // Need to wait an additional sample in case transportLayer collector runs before serverStatus.
    getNextSample();
    let data = getNextSample();
    assert(!data.hasOwnProperty("transportLayerStats"));

    setParameter("diagnosticDataCollectionMaxThreads", 2);

    let moreData = getNextSample();
    assert(moreData.hasOwnProperty("transportLayerStats"));

    fp.off();
}

function runTest(f) {
    waitUntilNormalOperation();
    f();
    resetParameters();
}

// Wait until FTDC is up and running before running any tests.
verifyGetDiagnosticData(adminDb);

runTest(testSampleTimeout);
runTest(testMinThreads);
runTest(testMaxThreads);

MongoRunner.stopMongod(conn);
