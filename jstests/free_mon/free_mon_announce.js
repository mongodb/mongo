// Validate connect message display.
//
load("jstests/free_mon/libs/free_mon.js");

(function() {
    'use strict';

    const mock_web = new FreeMonWebServer();
    mock_web.start();

    const merizod = MerizoRunner.runMerizod({
        setParameter: "cloudFreeMonitoringEndpointURL=" + mock_web.getURL(),
    });
    assert.neq(merizod, null, 'merizod not running');
    const admin = merizod.getDB('admin');

    function getConnectAnnounce() {
        // Capture message as it'd be presented to a user.
        clearRawMerizoProgramOutput();
        const exitCode = runMerizoProgram(
            'merizo', '--port', merizod.port, '--eval', "shellHelper( 'show', 'freeMonitoring' );");
        assert.eq(exitCode, 0);
        return rawMerizoProgramOutput();
    }

    // state === 'enabled'.
    admin.enableFreeMonitoring();
    WaitForRegistration(merizod);
    const reminder = "To see your monitoring data";
    assert.neq(getConnectAnnounce().search(reminder), -1, 'userReminder not found');

    // Cleanup.
    MerizoRunner.stopMerizod(merizod);
    mock_web.stop();
})();
