// Validate a user deleting free monitoring in system.version does
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

WaitForRegistration(rst.getPrimary());
WaitForRegistration(rst.getSecondary());

const qs1 = mock_web.queryStats();

jsTestLog("Breaking Free Monitoring");

// For kicks, delete the free monitoring storage state to knock free mon offline
// and make sure the node does not crash
rst.getPrimary().getDB("admin").system.version.remove({_id: "free_monitoring"});

jsTestLog("Sleeping for 20s");
sleep(20 * 1000);

const qs2 = mock_web.queryStats();

// Make sure we are back to the initial state.
WaitForFreeMonServerStatusState(rst.getPrimary(), 'undecided');
WaitForFreeMonServerStatusState(rst.getSecondary(), 'undecided');

// Verify free monitoring stops but tolerate one additional collection
assert.gte(qs1.metrics + 2, qs2.metrics);
// Tolerate an additional registration on the secondary side. We may delete the record on the
// primary before the secondary processes all pending registrations requests.
assert.lte(qs1.registers, qs2.registers);

// Enable it again to be sure we can resume
assert.commandWorked(rst.getPrimary().adminCommand({setFreeMonitoring: 1, action: "enable"}));
WaitForRegistration(rst.getPrimary());
WaitForRegistration(rst.getSecondary());

sleep(20 * 1000);

WaitForRegistration(rst.getPrimary());
WaitForRegistration(rst.getSecondary());

rst.stopSet();

mock_web.stop();
})();
