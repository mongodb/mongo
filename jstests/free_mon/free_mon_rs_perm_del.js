// Validate that if the endpoint says permanently delete that the state
// document is deleted and replicated properly
load("jstests/free_mon/libs/free_mon.js");

(function() {
    'use strict';

    let mock_web = new FreeMonWebServer(FAULT_PERMANENTLY_DELETE_AFTER_3, true);

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

    assert.eq(FreeMonGetStatus(rst.getPrimary()).state, 'enabled');
    assert.eq(FreeMonGetStatus(rst.getSecondary()).state, 'enabled');

    mock_web.enableFaults();
    mock_web.waitFaults(1);

    sleep(20 * 1000);

    // Make sure we are back to the initial state.
    assert.eq(FreeMonGetStatus(rst.getPrimary()).state, 'undecided');

    assert.eq(FreeMonGetStatus(rst.getSecondary()).state, 'undecided');

    // Disable the fault so we can re-enable again
    mock_web.disableFaults();

    // Enable it again to be sure we can resume
    assert.commandWorked(rst.getPrimary().adminCommand({setFreeMonitoring: 1, action: "enable"}));
    WaitForRegistration(rst.getPrimary());
    WaitForRegistration(rst.getSecondary());

    sleep(20 * 1000);

    assert.eq(FreeMonGetStatus(rst.getPrimary()).state, 'enabled');
    assert.eq(FreeMonGetStatus(rst.getSecondary()).state, 'enabled');

    rst.stopSet();

    mock_web.stop();
})();
