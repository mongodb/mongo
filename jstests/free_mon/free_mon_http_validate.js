// Ensure free monitoring gives up if registration fails
//
load("jstests/free_mon/libs/free_mon.js");

(function() {
'use strict';

let mock_web = new FreeMonWebServer(FAULT_INVALID_REGISTER);

mock_web.start();

let options = {
    setParameter: "cloudFreeMonitoringEndpointURL=" + mock_web.getURL(),
    enableFreeMonitoring: "on",
    verbose: 1,
};

const conn = MongoRunner.runMongod(options);
assert.neq(null, conn, 'mongod was unable to start up');

mock_web.waitRegisters(1);

// Sleep for some more time in case free monitoring would still try to register
sleep(20 * 1000);

// Ensure it only tried to register once since we gave it a bad response.
const stats = mock_web.queryStats();
print(tojson(stats));

assert.eq(stats.registers, 1);

MongoRunner.stopMongod(conn);

mock_web.stop();
})();
