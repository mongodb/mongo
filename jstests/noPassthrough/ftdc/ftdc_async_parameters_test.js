/**
 * Tests that async FTDC parameters behave properly.
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {verifyGetDiagnosticData, getNextSample} from "jstests/libs/ftdc.js";

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
    },
});
let adminDb = conn.getDB("admin");

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

function configureFailPointAndWaitUntilHit() {
    let fp = configureFailPoint(conn, "injectFTDCServerStatusCollectionDelay");
    fp.waitWithTimeout(kDefaultPeriod * 2);

    return fp;
}

function waitUntilNormalOperation() {
    // Since we are injecting delays into server status using the
    // injectFTDCServerStatusCollectionDelay failpoint, we define "normal" ftdc operation as having
    // data for serverStatus.
    assert.soon(
        () => {
            let data = assert.commandWorked(adminDb.runCommand("getDiagnosticData")).data;
            return data.hasOwnProperty("serverStatus");
        },
        "Timeout waiting for FTDC to operate normally.",
        90 * 1000,
    );
}

function testSampleTimeout() {
    jsTestLog("Running sample timeout test");

    // First test that we can't set the timeout too high.
    let invalidTimeoutCmd = {
        setParameter: 1,
        diagnosticDataCollectionSampleTimeoutMillis: kDefaultPeriod,
    };
    assert.commandFailedWithCode(adminDb.runCommand(invalidTimeoutCmd), ErrorCodes.InvalidOptions);

    // Set failpoint to block server status collection and wait until we can get a sample. This
    // sample should have a collection that timed out.
    let fp = configureFailPointAndWaitUntilHit();
    getNextSample(adminDb);
    const mongoOutput = rawMongoProgramOutput(".*");
    let logMatcher = /Collection timed out on collector/;
    assert(logMatcher.test(mongoOutput));

    // Disable the failpoint to stop FTDC from timing out. It's still possible that the next
    // collection can timeout if the time it takes to run the server code after the failpoint puts
    // the collection over the timeout threshold. This is why we get the next sample before clearing
    // logs and making the assertion.
    fp.off();
    getNextSample(adminDb);

    // By this point, we can safely clear the logs and check that the next sample completed with no
    // timeouts.
    clearRawMongoProgramOutput();
    getNextSample(adminDb);
    const moreMongoOutput = rawMongoProgramOutput(".*");
    assert(!logMatcher.test(moreMongoOutput));
}

function testMinThreads() {
    jsTestLog("Running min threads test");

    // First test that min threads can't be higher than max threads.
    const currentMax = adminDb.runCommand({
        getParameter: 1,
        diagnosticDataCollectionMaxThreads: 1,
    }).diagnosticDataCollectionMaxThreads;
    assert.commandFailedWithCode(
        adminDb.runCommand({setParameter: 1, diagnosticDataCollectionMinThreads: currentMax + 1}),
        ErrorCodes.BadValue,
    );

    setParameter("diagnosticDataCollectionMaxThreads", 4);
    setParameter("diagnosticDataCollectionMinThreads", 2);

    // Setting this failpoint blocks the serverStatus collector. Because we configured the minimum
    // thread count to 2, FTDC should spawn a new thread to continue collecting data instead of
    // blocking indefinitely. As a result, the next sample should contain data, but it will not
    // include serverStatus.
    let fp = configureFailPointAndWaitUntilHit();
    let data = getNextSample(adminDb);
    assert(data.hasOwnProperty("transportLayerStats"));
    assert(!data.hasOwnProperty("serverStatus"));

    fp.off();
}

function testMaxThreads() {
    jsTestLog("Running max threads test");

    // First test that max threads can't be lower than min threads.
    const currentMin = adminDb.runCommand({
        getParameter: 1,
        diagnosticDataCollectionMinThreads: 1,
    }).diagnosticDataCollectionMinThreads;
    assert.commandFailedWithCode(
        adminDb.runCommand({setParameter: 1, diagnosticDataCollectionMaxThreads: currentMin - 1}),
        ErrorCodes.BadValue,
    );

    let fp = configureFailPointAndWaitUntilHit();

    // With the failpoint set, we expect that ftdc should completely block since only one thread is in use.
    let data = getNextSample(adminDb);
    assert(!data.hasOwnProperty("transportLayerStats"));

    // Updating max threads will drain all ftdc collection work so we have to temporarily disable fp.
    fp.off();
    setParameter("diagnosticDataCollectionMaxThreads", 2);
    fp = configureFailPointAndWaitUntilHit();

    // Now with extra threads, ftdc collection can complete even with a blocking collection.
    let moreData = getNextSample(adminDb);
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
