// Validate connect message display.
//
load("jstests/free_mon/libs/free_mon.js");

(function() {
'use strict';

const mock_web = new FreeMonWebServer();
mock_web.start();

const mongod = MongoRunner.runMongod({
    setParameter: "cloudFreeMonitoringEndpointURL=" + mock_web.getURL(),
});
assert.neq(mongod, null, 'mongod not running');
const admin = mongod.getDB('admin');

// state === 'enabled'.
admin.enableFreeMonitoring();
WaitForRegistration(mongod);
const reminder = "To see your monitoring data";
assert(FreeMonGetStatus(mongod).userReminder.includes(reminder), 'userReminder not found');

// Cleanup.
MongoRunner.stopMongod(mongod);
mock_web.stop();
})();
