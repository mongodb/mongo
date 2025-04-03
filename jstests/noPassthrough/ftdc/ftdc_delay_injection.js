/**
 * Tests that injecting a delay into an FTDC collector will not prevent FTDC from collecting data.
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {verifyGetDiagnosticData} from "jstests/libs/ftdc.js";

let conn = MongoRunner.runMongod({
    setParameter: {
        featureFlagGaplessFTDC: true,
        diagnosticDataCollectionSampleTimeoutMillis: 200,
    }
});
let adminDb = conn.getDB('admin');

let data = verifyGetDiagnosticData(adminDb);
assert(data.hasOwnProperty("transportLayerStats"));
assert(data.hasOwnProperty("serverStatus"));

const fp =
    configureFailPoint(conn, "injectFTDCServerStatusCollectionDelay", {sleepTimeMillis: 10000});
fp.waitWithTimeout(2000);

// Since there is no guarantee on the order we run collectors, this sleep lets us test that we can
// still schedule and collect the results of transportLayerStats while serverStatus is actively
// blocking.
sleep(2000);

let dataAfterFp = assert.commandWorked(adminDb.runCommand("getDiagnosticData")).data;
assert(dataAfterFp.hasOwnProperty("transportLayerStats"));
assert(!dataAfterFp.hasOwnProperty("serverStatus"));

MongoRunner.stopMongod(conn);
