// Validate replica set starts up with free monitoring disabled
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

const rst = new ReplSetTest({nodes: 2, nodeOptions: options});

rst.startSet();
rst.initiate();
rst.awaitReplication();

const retStatus1 = rst.getPrimary().adminCommand({getFreeMonitoringStatus: 1});
assert.commandWorked(retStatus1);
assert.eq(retStatus1.state, "disabled", tojson(retStatus1));

const stats = mock_web.queryStats();
print(tojson(stats));

assert.eq(stats.registers, 0);

assert.commandFailed(rst.getPrimary().adminCommand({setFreeMonitoring: 1, action: "disable"}));

rst.stopSet();

mock_web.stop();
})();
