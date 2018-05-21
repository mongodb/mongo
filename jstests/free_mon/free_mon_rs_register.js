// Validate registration works in a replica set
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

    const rst = new ReplSetTest({nodes: 2, nodeOptions: options});
    rst.startSet();
    rst.initiate();
    rst.awaitReplication();

    sleep(10 * 1000);
    assert.eq(0, mock_web.queryStats().registers, "mongod registered without enabling free_mod");

    assert.commandWorked(rst.getPrimary().adminCommand({setFreeMonitoring: 1, action: "enable"}));
    WaitForRegistration(rst.getPrimary());

    mock_web.waitRegisters(2);

    const last_register = mock_web.query("last_register");
    print(tojson(last_register));

    assert.eq(last_register.version, 1);
    assert.eq(last_register.payload.buildInfo.bits, 64);
    assert.eq(last_register.payload.buildInfo.ok, 1);
    assert.eq(last_register.payload.storageEngine.readOnly, false);
    assert.eq(last_register.payload.isMaster.ok, 1);
    assert.eq(last_register.payload.replSetGetConfig.config.version, 2);

    function isUUID(val) {
        // Mock webserver gives us back unpacked BinData/UUID in the form:
        // { '$uuid': '0123456789abcdef0123456789abcdef' }.
        if ((typeof val) !== 'object') {
            return false;
        }
        const uuid = val['$uuid'];
        if ((typeof uuid) !== 'string') {
            return false;
        }
        return uuid.match(/^[0-9a-fA-F]{32}$/) !== null;
    }
    assert.eq(isUUID(last_register.payload.uuid['local.oplog.rs']), true);

    // Restart the secondary
    // Now we're going to shut down all nodes
    var s1 = rst._slaves[0];
    var s1Id = rst.getNodeId(s1);

    rst.stop(s1Id);
    rst.waitForState(s1, ReplSetTest.State.DOWN);

    rst.restart(s1Id);

    mock_web.waitRegisters(3);

    rst.stopSet();

    mock_web.stop();
})();
