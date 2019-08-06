// Validate resend registration works in a replica set
//
load("jstests/free_mon/libs/free_mon.js");

(function() {
'use strict';

let mock_web = new FreeMonWebServer(FAULT_RESEND_REGISTRATION_AT_3);
let mock_web_sec = new FreeMonWebServer(FAULT_RESEND_REGISTRATION_ONCE, true);

mock_web.start();
mock_web_sec.start();

const rst = new ReplSetTest({
    name: "free_mon_rs_register",
    nodes: [
        {
            setParameter: "cloudFreeMonitoringEndpointURL=" + mock_web.getURL(),
            verbose: 1,
        },
        {
            setParameter: "cloudFreeMonitoringEndpointURL=" + mock_web_sec.getURL(),
            verbose: 1,
        }
    ]
});

rst.startSet();
rst.initiate();
rst.awaitReplication();

sleep(10 * 1000);
assert.eq(0, mock_web.queryStats().registers, "mongod registered without enabling free_mod");
assert.eq(0, mock_web_sec.queryStats().registers, "mongod registered without enabling free_mod");

assert.commandWorked(rst.getPrimary().adminCommand({setFreeMonitoring: 1, action: "enable"}));
WaitForRegistration(rst.getPrimary());

mock_web.waitRegisters(1);
mock_web_sec.waitRegisters(1);

WaitForFreeMonServerStatusState(rst.getPrimary(), 'enabled');
WaitForFreeMonServerStatusState(rst.getSecondary(), 'enabled');

mock_web.waitRegisters(2);
mock_web_sec.waitRegisters(2);
mock_web_sec.disableFaults();

// Trigger resend on the secondary only
mock_web_sec.enableFaults();
mock_web_sec.waitFaults(1);
mock_web_sec.waitRegisters(3);

// Double check registers were as expected
const stats = mock_web.queryStats();
assert.eq(stats.registers, 2);

const stats_sec = mock_web_sec.queryStats();
assert.gte(stats_sec.registers, 3);

rst.stopSet();

mock_web.stop();
mock_web_sec.stop();
})();
