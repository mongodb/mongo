// Validate registration retries if the web server is down.
//
load("jstests/free_mon/libs/free_mon.js");

(function() {
'use strict';

let mock_web = new FreeMonWebServer(FAULT_FAIL_REGISTER);

mock_web.start();

let options = {
    setParameter: "cloudFreeMonitoringEndpointURL=" + mock_web.getURL(),
    enableFreeMonitoring: "on",
    verbose: 1,
};

const conn = MongoRunner.runMongod(options);
assert.neq(null, conn, 'mongod was unable to start up');
const admin = conn.getDB('admin');

mock_web.waitRegisters(3);

const freeMonStats = assert.commandWorked(admin.runCommand({serverStatus: 1})).freeMonitoring;
assert.gte(freeMonStats.registerErrors, 3);

MongoRunner.stopMongod(conn);

mock_web.stop();
})();
