// Validate registration works
//
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

    const conn = MongoRunner.runMongod(options);
    assert.neq(null, conn, 'mongod was unable to start up');

    WaitForRegistration(conn);

    const stats = mock_web.queryStats();
    print(tojson(stats));

    assert.eq(stats.registers, 1);

    const last_register = mock_web.query("last_register");
    print(tojson(last_register));

    assert.eq(last_register.version, 1);
    assert.eq(last_register.payload.buildInfo.bits, 64);
    assert.eq(last_register.payload.buildInfo.ok, 1);
    assert.eq(last_register.payload.storageEngine.readOnly, false);
    assert.eq(last_register.payload.isMaster.ok, 1);

    mock_web.waitMetrics(2);

    const last_metrics = mock_web.query("last_metrics");
    print(tojson(last_metrics));

    assert.eq(last_metrics.version, 1);

    MongoRunner.stopMongod(conn);

    mock_web.stop();
})();
