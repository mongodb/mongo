/**
 * Tests that the stub commands included in extension_stub_parsers.json model unloaded extension
 * commands by returning the specified error message rather than the generic CommandNotFound error.
 *
 * @tags: [featureFlagExtensionsAPI]
 */
import {
    checkPlatformCompatibleWithExtensions,
    withExtensions,
} from "jstests/noPassthrough/libs/extension_helpers.js";

checkPlatformCompatibleWithExtensions();

function assertStubCommandError(res) {
    assert.commandFailedWithCode(res, 13730200);
    assert.eq(
        res.errmsg,
        "The stubCommand command is a stub and does not perform any operations.",
        "unexpected stub command message",
        {res},
    );
}

// A stubbed command is still an unknown command, so it must still be counted in this metric.
function getUnknownCommandCount(conn) {
    return conn.getDB("admin").serverStatus().metrics.commands["<UNKNOWN>"];
}

function runStubCommandTests(conn) {
    const testDb = conn.getDB("test");

    // An unregistered stubbed command returns the stub's message, on any database.
    assertStubCommandError(testDb.runCommand({stubCommand: 1}));
    assertStubCommandError(conn.getDB("admin").runCommand({stubCommand: 1}));

    // An unregistered stubbed command run with explain returns the same error.
    assertStubCommandError(testDb.runCommand({explain: {stubCommand: 1}}));

    // A stubbed command still counts as an unknown command.
    const unknownCountBefore = getUnknownCommandCount(conn);
    assertStubCommandError(testDb.runCommand({stubCommand: 1}));
    assert.eq(getUnknownCommandCount(conn), unknownCountBefore + 1);

    // An unknown command without a stub still returns CommandNotFound.
    assert.commandFailedWithCode(
        testDb.runCommand({notAStubCommand: 1}),
        ErrorCodes.CommandNotFound,
    );
    assert.commandFailedWithCode(
        testDb.runCommand({explain: {notAStubCommand: 1}}),
        ErrorCodes.CommandNotFound,
    );
}

// With featureFlagExtensionCommands disabled, command stubs are not registered.
function runFlagDisabledTests(conn) {
    const testDb = conn.getDB("test");
    assert.commandFailedWithCode(testDb.runCommand({stubCommand: 1}), ErrorCodes.CommandNotFound);
    assert.commandFailedWithCode(
        testDb.runCommand({explain: {stubCommand: 1}}),
        ErrorCodes.CommandNotFound,
    );
}

withExtensions(
    {},
    runStubCommandTests,
    ["standalone", "sharded"],
    {},
    {
        setParameter: {featureFlagExtensionCommands: true},
    },
);

withExtensions(
    {},
    runFlagDisabledTests,
    ["standalone", "sharded"],
    {},
    {
        setParameter: {featureFlagExtensionCommands: false},
    },
);
