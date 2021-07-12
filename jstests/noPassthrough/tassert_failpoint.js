/**
 * Configures the failCommand failpoint to test the firing of a tripwire assertion (tassert).
 */
(function() {
'use strict';

let conn, testDB, adminDB;

const mongoRunnerSetupHelper = () => {
    conn = MongoRunner.runMongod({});
    testDB = conn.getDB("tassert_failpoint");
    adminDB = conn.getDB("admin");
};

/**
 * Helper for verifying the server exits with `exitCode`.
 */
const mongoRunnerExitHelper = (exitCode) => {
    assert.commandWorked(adminDB.runCommand({configureFailPoint: "failCommand", mode: "off"}));
    assert.eq(exitCode, MongoRunner.stopMongod(conn, null, {allowedExitCode: exitCode}));
};

// test with a tassert: true and closeConnection:true configuration. This should fire a tassert.
mongoRunnerSetupHelper();

assert.commandWorked(adminDB.runCommand({
    configureFailPoint: "failCommand",
    mode: "alwaysOn",
    data: {
        failCommands: ["ping"],
        closeConnection: true,
        tassert: true,
    }
}));

assert.throws(() => testDB.runCommand({ping: 1}));
mongoRunnerExitHelper(MongoRunner.EXIT_ABRUPT);

// test with a tassert:true and extraInfo:true configuration. This should fire a tassert.
mongoRunnerSetupHelper();

assert.commandWorked(adminDB.runCommand({
    configureFailPoint: "failCommand",
    mode: "alwaysOn",
    data: {
        errorCode: ErrorCodes.CannotImplicitlyCreateCollection,
        failCommands: ["create"],
        tassert: true,
        errorExtraInfo: {
            "ns": "namespace error",
        }
    }
}));

{
    let result = testDB.runCommand({create: "collection"});
    assert(result.ok == 0);
    assert(result.code == ErrorCodes.CannotImplicitlyCreateCollection);
    assert(result.ns == "namespace error");
}

mongoRunnerExitHelper(MongoRunner.EXIT_ABRUPT);

// test with a tassert:true and errorCode-only configuration. This should fire a tassert.
mongoRunnerSetupHelper();

assert.commandWorked(adminDB.runCommand({
    configureFailPoint: "failCommand",
    mode: "alwaysOn",
    data: {
        errorCode: ErrorCodes.InvalidNamespace,
        failCommands: ["ping"],
        tassert: true,
    }
}));

{
    let result = testDB.runCommand({ping: 1});
    assert(result.code == ErrorCodes.InvalidNamespace);
}

mongoRunnerExitHelper(MongoRunner.EXIT_ABRUPT);

// test with a tassert: false and closeConnection:true configuration. This should NOT fire a
// tassert.
mongoRunnerSetupHelper();

assert.commandWorked(adminDB.runCommand({
    configureFailPoint: "failCommand",
    mode: "alwaysOn",
    data: {
        failCommands: ["ping"],
        closeConnection: true,
        tassert: false,
    }
}));

assert.throws(() => testDB.runCommand({ping: 1}));
mongoRunnerExitHelper(MongoRunner.EXIT_CLEAN);

// test with a tassert:false and extraErrorInfo + errorCode configuration.
// This should NOT fire a tassert and should instead produce a uassert.
mongoRunnerSetupHelper();

assert.commandWorked(adminDB.runCommand({
    configureFailPoint: "failCommand",
    mode: "alwaysOn",
    data: {
        errorCode: ErrorCodes.CannotImplicitlyCreateCollection,
        failCommands: ["create"],
        tassert: false,
        errorExtraInfo: {
            "ns": "namespace error",
        }
    }
}));

{
    let result = testDB.runCommand({create: "collection"});
    assert(result.ok == 0);
    assert(result.code == ErrorCodes.CannotImplicitlyCreateCollection);
    assert(result.ns == "namespace error");
}
mongoRunnerExitHelper(MongoRunner.EXIT_CLEAN);

// test with a tassert:false and errorCode-only configuration.
// This should NOT fire a tassert and should instead produce a uassert..
mongoRunnerSetupHelper();

assert.commandWorked(adminDB.runCommand({
    configureFailPoint: "failCommand",
    mode: "alwaysOn",
    data: {
        errorCode: ErrorCodes.InvalidNamespace,
        failCommands: ["ping"],
        tassert: false,
    }
}));

{
    let result = testDB.runCommand({ping: 1});
    assert(result.code == ErrorCodes.InvalidNamespace);
}

mongoRunnerExitHelper(MongoRunner.EXIT_CLEAN);

// test with a tassert:true only configuration.
// This should NOT fire a tassert and should NOT produce an error.
// tassert should only be fired with one of the other settings.
mongoRunnerSetupHelper();

assert.commandWorked(adminDB.runCommand({
    configureFailPoint: "failCommand",
    mode: "alwaysOn",
    data: {
        failCommands: ["ping"],
        tassert: true,
    }
}));

assert.commandWorked(testDB.runCommand({ping: 1}));

mongoRunnerExitHelper(MongoRunner.EXIT_CLEAN);
})();
