// Validate a user manipulating system.version for free monitoring does
// not crash mongod
load("jstests/free_mon/libs/free_mon.js");

(function() {
'use strict';

let mock_web = new FreeMonWebServer();

mock_web.start();

let options = {
    setParameter: "cloudFreeMonitoringEndpointURL=" + mock_web.getURL(),
    enableFreeMonitoring: "on",
    verbose: 1,
};

const rst = new ReplSetTest({nodes: 2, nodeOptions: options});
rst.startSet();
rst.initiate();
rst.awaitReplication();

WaitForRegistration(rst.getPrimary());

mock_web.waitRegisters(2);

// For kicks, corrupt the free monitoring storage state to knock free mon offline
// and make sure the node does not crash
rst.getPrimary().getDB("admin").system.version.update({_id: "free_monitoring"},
                                                      {$set: {version: 2}});

sleep(20 * 1000);

rst.stopSet();

mock_web.stop();
})();
