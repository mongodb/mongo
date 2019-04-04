/**
 * Tests that failpoints can be set via --setParameter on the command line for merizos and merizod
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

    // In order to be able connect to a merizos that starts up successfully, start a config replica
    // set so that we can provide a valid config connection string to the merizos.
    var configRS = new ReplSetTest({nodes: 3});
    configRS.startSet({configsvr: '', storageEngine: 'wiredTiger'});
    configRS.initiate();

    // Setting a failpoint via --setParameter fails if enableTestCommands is not on.
    jsTest.setOption('enableTestCommands', false);
    assertStartupFails(
        MongoRunner.runMongod({setParameter: "failpoint.dummy=" + tojson(validFailpointPayload)}));
    assertStartupFails(MongoRunner.runMongos({
        setParameter: "failpoint.dummy=" + tojson(validFailpointPayload),
        configdb: configRS.getURL()
    }));
    jsTest.setOption('enableTestCommands', true);

    // Passing an invalid failpoint payload fails.
    assertStartupFails(MongoRunner.runMongod(
        {setParameter: "failpoint.dummy=" + tojson(invalidFailpointPayload)}));
    assertStartupFails(MongoRunner.runMongos({
        setParameter: "failpoint.dummy=" + tojson(invalidFailpointPayload),
        configdb: configRS.getURL()
    }));

    // Valid startup configurations succeed.
    var merizod =
        MongoRunner.runMongod({setParameter: "failpoint.dummy=" + tojson(validFailpointPayload)});
    assertStartupSucceeds(merizod);
    MongoRunner.stopMongod(merizod);

    var merizos = MongoRunner.runMongos({
        setParameter: "failpoint.dummy=" + tojson(validFailpointPayload),
        configdb: configRS.getURL()
    });
    assertStartupSucceeds(merizos);
    MongoRunner.stopMongos(merizos);

    merizod = MongoRunner.runMongod(
        {setParameter: "failpoint.dummy=" + tojson(validFailpointPayloadWithData)});
    assertStartupSucceeds(merizod);

    merizos = MongoRunner.runMongos({
        setParameter: "failpoint.dummy=" + tojson(validFailpointPayloadWithData),
        configdb: configRS.getURL()
    });
    assertStartupSucceeds(merizos);

    // The failpoint shows up with the correct data in the results of getParameter.

    var res = merizod.adminCommand({getParameter: "*"});
    assert.neq(null, res);
    assert.neq(null, res["failpoint.dummy"]);
    assert.eq(1, res["failpoint.dummy"].mode);  // the 'mode' is an enum internally; 'alwaysOn' is 1
    assert.eq(validFailpointPayloadWithData.data, res["failpoint.dummy"].data);

    res = merizos.adminCommand({getParameter: "*"});
    assert.neq(null, res);
    assert.neq(null, res["failpoint.dummy"]);
    assert.eq(1, res["failpoint.dummy"].mode);  // the 'mode' is an enum internally; 'alwaysOn' is 1
    assert.eq(validFailpointPayloadWithData.data, res["failpoint.dummy"].data);

    // The failpoint cannot be set by the setParameter command.
    assert.commandFailed(merizod.adminCommand({setParameter: 1, "dummy": validFailpointPayload}));
    assert.commandFailed(merizos.adminCommand({setParameter: 1, "dummy": validFailpointPayload}));

    // After changing the failpoint's state through the configureFailPoint command, the changes are
    // reflected in the output of the getParameter command.

    var newData = {x: 2};

    merizod.adminCommand({configureFailPoint: "dummy", mode: "alwaysOn", data: newData});
    res = merizod.adminCommand({getParameter: 1, "failpoint.dummy": 1});
    assert.neq(null, res);
    assert.neq(null, res["failpoint.dummy"]);
    assert.eq(1, res["failpoint.dummy"].mode);  // the 'mode' is an enum internally; 'alwaysOn' is 1
    assert.eq(newData, res["failpoint.dummy"].data);

    merizos.adminCommand({configureFailPoint: "dummy", mode: "alwaysOn", data: newData});
    res = merizos.adminCommand({getParameter: 1, "failpoint.dummy": 1});
    assert.neq(null, res);
    assert.neq(null, res["failpoint.dummy"]);
    assert.eq(1, res["failpoint.dummy"].mode);  // the 'mode' is an enum internally; 'alwaysOn' is 1
    assert.eq(newData, res["failpoint.dummy"].data);

    MongoRunner.stopMongod(merizod);
    MongoRunner.stopMongos(merizos);

    // Failpoint server parameters do not show up in the output of getParameter when not running
    // with enableTestCommands=1.

    jsTest.setOption('enableTestCommands', false);
    TestData.roleGraphInvalidationIsFatal = false;

    merizod = MongoRunner.runMongod();
    assertStartupSucceeds(merizod);

    merizos = MongoRunner.runMongos({configdb: configRS.getURL()});
    assertStartupSucceeds(merizos);

    // Doing getParameter for a specific failpoint fails.
    assert.commandFailed(merizod.adminCommand({getParameter: 1, "failpoint.dummy": 1}));
    assert.commandFailed(merizos.adminCommand({getParameter: 1, "failpoint.dummy": 1}));

    // No failpoint parameters show up when listing all parameters through getParameter.
    res = merizod.adminCommand({getParameter: "*"});
    assert.neq(null, res);
    for (var parameter in res) {  // for-in loop valid only for top-level field checks.
        assert(!parameter.includes("failpoint."));
    }

    res = merizos.adminCommand({getParameter: "*"});
    assert.neq(null, res);
    for (var parameter in res) {  // for-in loop valid only for top-level field checks.
        assert(!parameter.includes("failpoint."));
    }

    MongoRunner.stopMongod(merizod);
    MongoRunner.stopMongos(merizos);
    configRS.stopSet();
})();
