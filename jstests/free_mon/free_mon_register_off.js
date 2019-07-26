// Validate registration does work if free monitoring is disabled.
//
load("jstests/free_mon/libs/free_mon.js");

(function() {
'use strict';

let mock_web = new FreeMonWebServer();

mock_web.start();

let options = {
    setParameter: "cloudFreeMonitoringEndpointURL=" + mock_web.getURL(),
    enableFreeMonitoring: "off",
    verbose: 1,
};

const conn = MongoRunner.runMongod(options);
assert.neq(null, conn, 'mongod was unable to start up');

assert.commandFailed(conn.adminCommand({setFreeMonitoring: 1, action: "enable"}));

// If it some time in case it actually started to process something.
sleep(10 * 1000);

const retStatus1 = conn.adminCommand({getFreeMonitoringStatus: 1});
assert.commandWorked(retStatus1);
assert.eq(retStatus1.state, "disabled", tojson(retStatus1));

const stats = mock_web.queryStats();
print(tojson(stats));

assert.eq(stats.registers, 0);

assert.commandFailed(conn.adminCommand({setFreeMonitoring: 1, action: "disable"}));

MongoRunner.stopMongod(conn);

mock_web.stop();
})();
