/**
 * Tests that failpoints can be set via --setParameter on the command line for bongos and bongod
 * only when running with enableTestCommands=1.
 */
(function() {

    "use strict";

    var assertStartupSucceeds = function(conn) {
        assert.commandWorked(conn.adminCommand({ismaster: 1}));
    };

    var assertStartupFails = function(conn) {
        assert.eq(null, conn);
    };

    var validFailpointPayload = {'mode': 'alwaysOn'};
    var validFailpointPayloadWithData = {'mode': 'alwaysOn', 'data': {x: 1}};
    var invalidFailpointPayload = "notJSON";

    // In order to be able connect to a bongos that starts up successfully, start a config replica
    // set so that we can provide a valid config connection string to the bongos.
    var configRS = new ReplSetTest({nodes: 3});
    configRS.startSet({configsvr: '', storageEngine: 'wiredTiger'});
    configRS.initiate();

    // Setting a failpoint via --setParameter fails if enableTestCommands is not on.
    jsTest.setOption('enableTestCommands', false);
    assertStartupFails(
        BongoRunner.runBongod({setParameter: "failpoint.dummy=" + tojson(validFailpointPayload)}));
    assertStartupFails(BongoRunner.runBongos({
        setParameter: "failpoint.dummy=" + tojson(validFailpointPayload),
        configdb: configRS.getURL()
    }));
    jsTest.setOption('enableTestCommands', true);

    // Passing an invalid failpoint payload fails.
    assertStartupFails(BongoRunner.runBongod(
        {setParameter: "failpoint.dummy=" + tojson(invalidFailpointPayload)}));
    assertStartupFails(BongoRunner.runBongos({
        setParameter: "failpoint.dummy=" + tojson(invalidFailpointPayload),
        configdb: configRS.getURL()
    }));

    // Valid startup configurations succeed.
    var bongod =
        BongoRunner.runBongod({setParameter: "failpoint.dummy=" + tojson(validFailpointPayload)});
    assertStartupSucceeds(bongod);
    BongoRunner.stopBongod(bongod);

    var bongos = BongoRunner.runBongos({
        setParameter: "failpoint.dummy=" + tojson(validFailpointPayload),
        configdb: configRS.getURL()
    });
    assertStartupSucceeds(bongos);
    BongoRunner.stopBongos(bongos);

    bongod = BongoRunner.runBongod(
        {setParameter: "failpoint.dummy=" + tojson(validFailpointPayloadWithData)});
    assertStartupSucceeds(bongod);

    bongos = BongoRunner.runBongos({
        setParameter: "failpoint.dummy=" + tojson(validFailpointPayloadWithData),
        configdb: configRS.getURL()
    });
    assertStartupSucceeds(bongos);

    // The failpoint shows up with the correct data in the results of getParameter.

    var res = bongod.adminCommand({getParameter: "*"});
    assert.neq(null, res);
    assert.neq(null, res["failpoint.dummy"]);
    assert.eq(1, res["failpoint.dummy"].mode);  // the 'mode' is an enum internally; 'alwaysOn' is 1
    assert.eq(validFailpointPayloadWithData.data, res["failpoint.dummy"].data);

    res = bongos.adminCommand({getParameter: "*"});
    assert.neq(null, res);
    assert.neq(null, res["failpoint.dummy"]);
    assert.eq(1, res["failpoint.dummy"].mode);  // the 'mode' is an enum internally; 'alwaysOn' is 1
    assert.eq(validFailpointPayloadWithData.data, res["failpoint.dummy"].data);

    // The failpoint cannot be set by the setParameter command.
    assert.commandFailed(bongod.adminCommand({setParameter: 1, "dummy": validFailpointPayload}));
    assert.commandFailed(bongos.adminCommand({setParameter: 1, "dummy": validFailpointPayload}));

    // After changing the failpoint's state through the configureFailPoint command, the changes are
    // reflected in the output of the getParameter command.

    var newData = {x: 2};

    bongod.adminCommand({configureFailPoint: "dummy", mode: "alwaysOn", data: newData});
    res = bongod.adminCommand({getParameter: 1, "failpoint.dummy": 1});
    assert.neq(null, res);
    assert.neq(null, res["failpoint.dummy"]);
    assert.eq(1, res["failpoint.dummy"].mode);  // the 'mode' is an enum internally; 'alwaysOn' is 1
    assert.eq(newData, res["failpoint.dummy"].data);

    bongos.adminCommand({configureFailPoint: "dummy", mode: "alwaysOn", data: newData});
    res = bongos.adminCommand({getParameter: 1, "failpoint.dummy": 1});
    assert.neq(null, res);
    assert.neq(null, res["failpoint.dummy"]);
    assert.eq(1, res["failpoint.dummy"].mode);  // the 'mode' is an enum internally; 'alwaysOn' is 1
    assert.eq(newData, res["failpoint.dummy"].data);

    BongoRunner.stopBongod(bongod);
    BongoRunner.stopBongos(bongos);

    // Failpoint server parameters do not show up in the output of getParameter when not running
    // with enableTestCommands=1.

    jsTest.setOption('enableTestCommands', false);

    bongod = BongoRunner.runBongod();
    assertStartupSucceeds(bongod);

    bongos = BongoRunner.runBongos({configdb: configRS.getURL()});
    assertStartupSucceeds(bongos);

    // Doing getParameter for a specific failpoint fails.
    assert.commandFailed(bongod.adminCommand({getParameter: 1, "failpoint.dummy": 1}));
    assert.commandFailed(bongos.adminCommand({getParameter: 1, "failpoint.dummy": 1}));

    // No failpoint parameters show up when listing all parameters through getParameter.
    res = bongod.adminCommand({getParameter: "*"});
    assert.neq(null, res);
    for (var parameter in res) {  // for-in loop valid only for top-level field checks.
        assert(!parameter.includes("failpoint."));
    }

    res = bongos.adminCommand({getParameter: "*"});
    assert.neq(null, res);
    for (var parameter in res) {  // for-in loop valid only for top-level field checks.
        assert(!parameter.includes("failpoint."));
    }

    BongoRunner.stopBongod(bongod);
    BongoRunner.stopBongos(bongos);
    configRS.stopSet();
})();
