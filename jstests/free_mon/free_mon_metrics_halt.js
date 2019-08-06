// Ensure free monitoring gives up if metrics returns halt
//
load("jstests/free_mon/libs/free_mon.js");

(function() {
'use strict';

let mock_web = new FreeMonWebServer(FAULT_HALT_METRICS_5);

mock_web.start();

let options = {
    setParameter: "cloudFreeMonitoringEndpointURL=" + mock_web.getURL(),
    enableFreeMonitoring: "on",
    verbose: 1,
};

const conn = MongoRunner.runMongod(options);
assert.neq(null, conn, 'mongod was unable to start up');

mock_web.waitMetrics(6);

// It gets marked as disabled on halt
WaitForUnRegistration(conn);

MongoRunner.stopMongod(conn);

mock_web.stop();
})();
