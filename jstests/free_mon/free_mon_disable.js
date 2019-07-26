// Validate disable works
//
load("jstests/free_mon/libs/free_mon.js");

(function() {
'use strict';

let mock_web = new FreeMonWebServer();

mock_web.start();

let options = {
    setParameter: "cloudFreeMonitoringEndpointURL=" + mock_web.getURL(),
    freeMonitoringTag: "foo",
    verbose: 1,
};

const conn = MongoRunner.runMongod(options);
assert.neq(null, conn, 'mongod was unable to start up');

assert.commandWorked(conn.adminCommand({setFreeMonitoring: 1, action: "disable"}));

const stats = mock_web.queryStats();
print(tojson(stats));

assert.eq(stats.registers, 0);

assert.eq(FreeMonGetStatus(conn).state, "disabled");

assert.eq(FreeMonGetServerStatus(conn).state, "disabled");

MongoRunner.stopMongod(conn);

mock_web.stop();
})();
