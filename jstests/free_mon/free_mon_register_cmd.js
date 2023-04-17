// Validate registration works via a command. Validate it can be registered and unregistered
//
load("jstests/free_mon/libs/free_mon.js");

(function() {
'use strict';

let mock_web = new FreeMonWebServer();

mock_web.start();

let options = {
    setParameter: "cloudFreeMonitoringEndpointURL=" + mock_web.getURL(),
    verbose: 1,
};

const conn = MongoRunner.runMongod(options);
assert.neq(null, conn, 'mongod was unable to start up');

// Wait an arbitrary amount of time to allow the processor loop to start.
sleep(10 * 1000);

// Then verify that no registrations happened since we haven't runtime enabled yed.
assert.eq('undecided', FreeMonGetStatus(conn).state, "Initial state should be 'undecided'");
assert.eq(0, mock_web.queryStats().registers, "mongod registered without enabling free_mod");

assert.commandWorked(conn.adminCommand({setFreeMonitoring: 1, action: "enable"}));

WaitForFreeMonServerStatusState(conn, 'enabled');

// The command should either timeout or suceed after registration is complete
const retStatus1 = FreeMonGetStatus(conn);
assert.eq(retStatus1.state, "enabled", tojson(retStatus1));

const stats = mock_web.queryStats();
print(tojson(stats));

assert.eq(stats.registers, 1);

const last_register = mock_web.query("last_register");
print(tojson(last_register));

assert.eq(last_register.version, 2);
assert.eq(last_register.payload.buildInfo.bits, 64);
assert.eq(last_register.payload.buildInfo.ok, 1);
assert.eq(last_register.payload.storageEngine.readOnly, false);
assert.eq(last_register.payload.isMaster.ok, 1);

mock_web.waitMetrics(2);

const last_metrics = mock_web.query("last_metrics");
print(tojson(last_metrics));

assert.eq(last_metrics.version, 2);

assert.commandWorked(conn.adminCommand({setFreeMonitoring: 1, action: "disable"}));

// Wait for unregistration to occur
assert.soon(function() {
    const regDoc = FreeMonGetRegistration(conn);
    return regDoc.state == "disabled";
}, "Failed to unregister", 60 * 1000);

const retStatus2 = FreeMonGetStatus(conn);
assert.eq(retStatus2.state, "disabled", tojson(retStatus1));

MongoRunner.stopMongod(conn);

mock_web.stop();
})();
