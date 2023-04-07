/**
 * This test is to make sure that 'apiVersions' section is omitted from serverStatus metrics in
 * FTDC data.
 */
load('jstests/libs/ftdc.js');

(function() {
'use strict';
let conn = MongoRunner.runMongod();
let adminDb = conn.getDB('admin');

// Verify 'apiVersions' section is omitted from serverStatus metrics.
let ftdcData = verifyGetDiagnosticData(adminDb);
assert(ftdcData["serverStatus"].hasOwnProperty("metrics"),
       "does not have 'serverStatus.metrics' in '" + tojson(ftdcData) + "'");
assert(!ftdcData["serverStatus"]["metrics"].hasOwnProperty("apiVersions"),
       "'serverStatus.metrics.apiVersions' should be omitted from FTDC data: '" + tojson(ftdcData) +
           "'");

// Make sure that 'apiVersions' section still be returned with serverStatus metrics.
let serverStatusMetrics = adminDb.serverStatus().metrics;
assert(serverStatusMetrics.hasOwnProperty("apiVersions"),
       "does not have 'apiVersions' in '" + tojson(serverStatusMetrics) + "'");

MongoRunner.stopMongod(conn);
})();
