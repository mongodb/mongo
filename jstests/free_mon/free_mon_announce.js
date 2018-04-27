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

    function getConnectAnnounce() {
        // Capture message as it'd be presented to a user.
        clearRawMongoProgramOutput();
        const exitCode = runMongoProgram('mongo', '--port', mongod.port, '--eval', ';');
        assert.eq(exitCode, 0);
        return rawMongoProgramOutput();
    }

    // state === 'enabled'.
    admin.enableFreeMonitoring();
    WaitForRegistration(mongod);
    const reminder = "Don't forget to check your metrics";
    assert.neq(getConnectAnnounce().search(reminder), -1, 'userReminder not found');

    // Cleanup.
    MongoRunner.stopMongod(mongod);
    mock_web.stop();
})();
