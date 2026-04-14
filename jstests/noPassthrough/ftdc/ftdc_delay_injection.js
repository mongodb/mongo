/**
 * Tests that forcing a FTDC collector to block will not prevent FTDC from collecting data.
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {verifyGetDiagnosticData, getNextSample} from "jstests/libs/ftdc.js";

const kDefaultPeriod = 1000;

let conn = MongoRunner.runMongod({
    setParameter: {
        featureFlagGaplessFTDC: true,
        diagnosticDataCollectionSampleTimeoutMillis: 200,
    },
});
let adminDb = conn.getDB("admin");

// Wait until FTDC is up and running before running any tests.
verifyGetDiagnosticData(adminDb);

let data = getNextSample(adminDb);
assert(data.hasOwnProperty("transportLayerStats"));
assert(data.hasOwnProperty("serverStatus")); // server status collector's result is included.

const fp = configureFailPoint(conn, "injectFTDCServerStatusCollectionDelay");
fp.waitWithTimeout(kDefaultPeriod * 2);

let dataAfterFp = getNextSample(adminDb);
assert(dataAfterFp.hasOwnProperty("transportLayerStats"));
assert(!dataAfterFp.hasOwnProperty("serverStatus")); // server status collector's result is not included, because that collector timed out

fp.off();
MongoRunner.stopMongod(conn);
