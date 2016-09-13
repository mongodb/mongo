/**
 * Tests that failpoints can be set via --setParameter on the command line for mongos and mongod
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

    // In order to be able connect to a mongos that starts up successfully, start a config replica
    // set so that we can provide a valid config connection string to the mongos.
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
    var mongod =
        MongoRunner.runMongod({setParameter: "failpoint.dummy=" + tojson(validFailpointPayload)});
    assertStartupSucceeds(mongod);
    MongoRunner.stopMongod(mongod);

    var mongos = MongoRunner.runMongos({
        setParameter: "failpoint.dummy=" + tojson(validFailpointPayload),
        configdb: configRS.getURL()
    });
    assertStartupSucceeds(mongos);
    MongoRunner.stopMongos(mongos);

    mongod = MongoRunner.runMongod(
        {setParameter: "failpoint.dummy=" + tojson(validFailpointPayloadWithData)});
    assertStartupSucceeds(mongod);

    mongos = MongoRunner.runMongos({
        setParameter: "failpoint.dummy=" + tojson(validFailpointPayloadWithData),
        configdb: configRS.getURL()
    });
    assertStartupSucceeds(mongos);

    // The failpoint shows up with the correct data in the results of getParameter.

    var res = mongod.adminCommand({getParameter: "*"});
    assert.neq(null, res);
    assert.neq(null, res["failpoint.dummy"]);
    assert.eq(1, res["failpoint.dummy"].mode);  // the 'mode' is an enum internally; 'alwaysOn' is 1
    assert.eq(validFailpointPayloadWithData.data, res["failpoint.dummy"].data);

    res = mongos.adminCommand({getParameter: "*"});
    assert.neq(null, res);
    assert.neq(null, res["failpoint.dummy"]);
    assert.eq(1, res["failpoint.dummy"].mode);  // the 'mode' is an enum internally; 'alwaysOn' is 1
    assert.eq(validFailpointPayloadWithData.data, res["failpoint.dummy"].data);

    // The failpoint cannot be set by the setParameter command.
    assert.commandFailed(mongod.adminCommand({setParameter: 1, "dummy": validFailpointPayload}));
    assert.commandFailed(mongos.adminCommand({setParameter: 1, "dummy": validFailpointPayload}));

    // After changing the failpoint's state through the configureFailPoint command, the changes are
    // reflected in the output of the getParameter command.

    var newData = {x: 2};

    mongod.adminCommand({configureFailPoint: "dummy", mode: "alwaysOn", data: newData});
    res = mongod.adminCommand({getParameter: 1, "failpoint.dummy": 1});
    assert.neq(null, res);
    assert.neq(null, res["failpoint.dummy"]);
    assert.eq(1, res["failpoint.dummy"].mode);  // the 'mode' is an enum internally; 'alwaysOn' is 1
    assert.eq(newData, res["failpoint.dummy"].data);

    mongos.adminCommand({configureFailPoint: "dummy", mode: "alwaysOn", data: newData});
    res = mongos.adminCommand({getParameter: 1, "failpoint.dummy": 1});
    assert.neq(null, res);
    assert.neq(null, res["failpoint.dummy"]);
    assert.eq(1, res["failpoint.dummy"].mode);  // the 'mode' is an enum internally; 'alwaysOn' is 1
    assert.eq(newData, res["failpoint.dummy"].data);

    MongoRunner.stopMongod(mongod);
    MongoRunner.stopMongos(mongos);

    // Failpoint server parameters do not show up in the output of getParameter when not running
    // with enableTestCommands=1.

    jsTest.setOption('enableTestCommands', false);

    mongod = MongoRunner.runMongod();
    assertStartupSucceeds(mongod);

    mongos = MongoRunner.runMongos({configdb: configRS.getURL()});
    assertStartupSucceeds(mongos);

    // Doing getParameter for a specific failpoint fails.
    assert.commandFailed(mongod.adminCommand({getParameter: 1, "failpoint.dummy": 1}));
    assert.commandFailed(mongos.adminCommand({getParameter: 1, "failpoint.dummy": 1}));

    // No failpoint parameters show up when listing all parameters through getParameter.
    res = mongod.adminCommand({getParameter: "*"});
    assert.neq(null, res);
    for (var parameter in res) {  // for-in loop valid only for top-level field checks.
        assert(!parameter.includes("failpoint."));
    }

    res = mongos.adminCommand({getParameter: "*"});
    assert.neq(null, res);
    for (var parameter in res) {  // for-in loop valid only for top-level field checks.
        assert(!parameter.includes("failpoint."));
    }

    MongoRunner.stopMongod(mongod);
    MongoRunner.stopMongos(mongos);
    configRS.stopSet();
})();
