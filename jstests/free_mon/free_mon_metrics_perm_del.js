// Ensure free monitoring gives up if metrics returns permanently delete
//
load("jstests/free_mon/libs/free_mon.js");

(function() {
'use strict';

let mock_web = new FreeMonWebServer(FAULT_PERMANENTLY_DELETE_AFTER_3);

mock_web.start();

let options = {
    setParameter: "cloudFreeMonitoringEndpointURL=" + mock_web.getURL(),
    enableFreeMonitoring: "on",
    verbose: 1,
};

const conn = MongoRunner.runMongod(options);
assert.neq(null, conn, 'mongod was unable to start up');

mock_web.waitMetrics(4);

// Make sure the registration document gets removed
const reg = FreeMonGetRegistration(conn);
print(tojson(reg));
assert.eq(reg, undefined);

MongoRunner.stopMongod(conn);

mock_web.stop();
})();
