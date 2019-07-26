// Validate resend registration works in a replica set
//
load("jstests/free_mon/libs/free_mon.js");

(function() {
'use strict';

let mock_web = new FreeMonWebServer(FAULT_RESEND_REGISTRATION_ONCE);

mock_web.start();

let options = {
    setParameter: "cloudFreeMonitoringEndpointURL=" + mock_web.getURL(),
    enableFreeMonitoring: "on",
    verbose: 1,
};

const conn = MongoRunner.runMongod(options);
assert.neq(null, conn, 'mongod was unable to start up');

WaitForRegistration(conn);

mock_web.waitRegisters(2);

MongoRunner.stopMongod(conn);

mock_web.stop();
})();
