// Tests the "failCommand" failpoint.
(function() {
    "use strict";

    const conn = MongoRunner.runMongod();
    assert.neq(null, conn, "mongod was unable to start up");
    const testDB = conn.getDB("test");

    // Test failing with a particular error code.
    assert.commandWorked(conn.adminCommand({
        configureFailPoint: "failCommand",
        mode: "alwaysOn",
        data: {errorCode: ErrorCodes.NotMaster}
    }));
    assert.commandFailedWithCode(testDB.runCommand({find: "c"}), ErrorCodes.NotMaster);
    assert.commandWorked(conn.adminCommand({configureFailPoint: "failCommand", mode: "off"}));

    // Test ignored commands when failing with a particular error code.
    assert.commandWorked(conn.adminCommand({
        configureFailPoint: "failCommand",
        mode: "alwaysOn",
        data: {errorCode: ErrorCodes.BadValue}
    }));
    assert.commandFailedWithCode(testDB.runCommand({find: "c"}), ErrorCodes.BadValue);
    assert.commandWorked(testDB.runCommand({isMaster: 1}));
    assert.commandWorked(testDB.runCommand({buildinfo: 1}));
    assert.commandWorked(testDB.runCommand({ping: 1}));
    assert.commandWorked(conn.adminCommand({configureFailPoint: "failCommand", mode: "off"}));

    // Test skip when failing with a particular error code.
    assert.commandWorked(conn.adminCommand({
        configureFailPoint: "failCommand",
        mode: {skip: 2},
        data: {errorCode: ErrorCodes.NotMaster}
    }));
    assert.commandWorked(testDB.runCommand({find: "c"}));
    assert.commandWorked(testDB.runCommand({find: "c"}));
    assert.commandFailedWithCode(testDB.runCommand({find: "c"}), ErrorCodes.NotMaster);
    assert.commandWorked(conn.adminCommand({configureFailPoint: "failCommand", mode: "off"}));

    // Test times when failing with a particular error code.
    assert.commandWorked(conn.adminCommand({
        configureFailPoint: "failCommand",
        mode: {times: 2},
        data: {errorCode: ErrorCodes.NotMaster}
    }));
    assert.commandFailedWithCode(testDB.runCommand({find: "c"}), ErrorCodes.NotMaster);
    assert.commandFailedWithCode(testDB.runCommand({find: "c"}), ErrorCodes.NotMaster);
    assert.commandWorked(testDB.runCommand({find: "c"}));
    assert.commandWorked(conn.adminCommand({configureFailPoint: "failCommand", mode: "off"}));

    // Ignored commands are not counted for skip.
    assert.commandWorked(conn.adminCommand({
        configureFailPoint: "failCommand",
        mode: {skip: 1},
        data: {errorCode: ErrorCodes.BadValue}
    }));
    assert.commandWorked(testDB.runCommand({isMaster: 1}));
    assert.commandWorked(testDB.runCommand({buildinfo: 1}));
    assert.commandWorked(testDB.runCommand({ping: 1}));
    assert.commandWorked(testDB.runCommand({find: "c"}));
    assert.commandFailedWithCode(testDB.runCommand({find: "c"}), ErrorCodes.BadValue);
    assert.commandWorked(conn.adminCommand({configureFailPoint: "failCommand", mode: "off"}));

    // Ignored commands are not counted for times.
    assert.commandWorked(conn.adminCommand({
        configureFailPoint: "failCommand",
        mode: {times: 1},
        data: {errorCode: ErrorCodes.BadValue}
    }));
    assert.commandWorked(testDB.runCommand({isMaster: 1}));
    assert.commandWorked(testDB.runCommand({buildinfo: 1}));
    assert.commandWorked(testDB.runCommand({ping: 1}));
    assert.commandFailedWithCode(testDB.runCommand({find: "c"}), ErrorCodes.BadValue);
    assert.commandWorked(testDB.runCommand({find: "c"}));
    assert.commandWorked(conn.adminCommand({configureFailPoint: "failCommand", mode: "off"}));

    // Test closing connection.
    assert.commandWorked(conn.adminCommand(
        {configureFailPoint: "failCommand", mode: "alwaysOn", data: {closeConnection: true}}));
    assert.throws(() => testDB.runCommand({find: "c"}));
    assert.commandWorked(conn.adminCommand({configureFailPoint: "failCommand", mode: "off"}));

    // Test ignored commands when closing the connection.
    assert.commandWorked(conn.adminCommand(
        {configureFailPoint: "failCommand", mode: "alwaysOn", data: {closeConnection: true}}));
    assert.commandWorked(testDB.runCommand({isMaster: 1}));
    assert.commandWorked(testDB.runCommand({buildinfo: 1}));
    assert.commandWorked(testDB.runCommand({ping: 1}));
    assert.throws(() => testDB.runCommand({find: "c"}));
    assert.commandWorked(conn.adminCommand({configureFailPoint: "failCommand", mode: "off"}));

    // Test skip when closing connection.
    assert.commandWorked(conn.adminCommand(
        {configureFailPoint: "failCommand", mode: {skip: 2}, data: {closeConnection: true}}));
    assert.commandWorked(testDB.runCommand({find: "c"}));
    assert.commandWorked(testDB.runCommand({find: "c"}));
    assert.throws(() => testDB.runCommand({find: "c"}));
    assert.commandWorked(conn.adminCommand({configureFailPoint: "failCommand", mode: "off"}));

    // Test times when closing connection.
    assert.commandWorked(conn.adminCommand(
        {configureFailPoint: "failCommand", mode: {times: 2}, data: {closeConnection: true}}));
    assert.throws(() => testDB.runCommand({find: "c"}));
    assert.throws(() => testDB.runCommand({find: "c"}));
    assert.commandWorked(testDB.runCommand({find: "c"}));
    assert.commandWorked(conn.adminCommand({configureFailPoint: "failCommand", mode: "off"}));

    // Ignored commands are not counted for skip.
    assert.commandWorked(conn.adminCommand(
        {configureFailPoint: "failCommand", mode: {skip: 1}, data: {closeConnection: true}}));
    assert.commandWorked(testDB.runCommand({isMaster: 1}));
    assert.commandWorked(testDB.runCommand({buildinfo: 1}));
    assert.commandWorked(testDB.runCommand({ping: 1}));
    assert.commandWorked(testDB.runCommand({find: "c"}));
    assert.throws(() => testDB.runCommand({find: "c"}));
    assert.commandWorked(conn.adminCommand({configureFailPoint: "failCommand", mode: "off"}));

    // Ignored commands are not counted for times.
    assert.commandWorked(conn.adminCommand(
        {configureFailPoint: "failCommand", mode: {times: 1}, data: {closeConnection: true}}));
    assert.commandWorked(testDB.runCommand({isMaster: 1}));
    assert.commandWorked(testDB.runCommand({buildinfo: 1}));
    assert.commandWorked(testDB.runCommand({ping: 1}));
    assert.throws(() => testDB.runCommand({find: "c"}));
    assert.commandWorked(testDB.runCommand({find: "c"}));
    assert.commandWorked(conn.adminCommand({configureFailPoint: "failCommand", mode: "off"}));

    MongoRunner.stopMongod(conn);
}());
