// Validate serverStatus output.
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

    const kRetryIntervalSecs = 1;
    function freeMonStats() {
        return assert.commandWorked(admin.runCommand({serverStatus: 1})).freeMonitoring;
    }

    // Initial state.
    assert.eq(freeMonStats().state, 'undecided');

    admin.enableFreeMonitoring();
    WaitForRegistration(mongod);

    // Enabled.
    const enabled = freeMonStats();
    assert.eq(enabled.state, 'enabled');
    assert.eq(enabled.retryIntervalSecs, kRetryIntervalSecs);
    assert.eq(enabled.registerErrors, 0);
    assert.eq(enabled.metricsErrors, 0);

    // Explicitly disabled.
    admin.disableFreeMonitoring();

    WaitForFreeMonServerStatusState(mongod, "disabled");

    const disabled = freeMonStats();
    assert.eq(disabled.state, 'disabled');
    assert.eq(disabled.retryIntervalSecs, kRetryIntervalSecs);
    assert.eq(disabled.registerErrors, 0);
    assert.eq(disabled.metricsErrors, 0);

    // Cleanup.
    MongoRunner.stopMongod(mongod);
    mock_web.stop();
})();
