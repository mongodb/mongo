/**
 * Tests that an extension command receives the request and the command context, and that its
 * response reaches the client.
 * @tags: [featureFlagExtensionsAPI, featureFlagExtensionCommands]
 */
import {describe, it} from "jstests/libs/mochalite.js";
import {
    checkPlatformCompatibleWithExtensions,
    withExtensions,
} from "jstests/noPassthrough/libs/extension_helpers.js";

checkPlatformCompatibleWithExtensions();

const extensions = {"libmock_commands_extension.so": {}};

function testEcho(conn) {
    const db = conn.getDB(jsTestName());

    let res = assert.commandWorked(db.runCommand({testEcho: "coll", message: "hello"}));
    assert.eq(res.request.message, "hello", {res});
    assert.eq(res.db, db.getName(), {res});
    assert.eq(res.coll, "coll", {res});

    // A command that does not name a collection passes an empty collection name.
    res = assert.commandWorked(db.runCommand({testEcho: 1}));
    assert.eq(res.db, db.getName(), {res});
    assert.eq(res.coll, "", {res});
}

describe("testEcho extension command", function () {
    // testEcho declares 'clusterRole: both', so it runs on mongod (shard role) and on mongos
    // (router role).
    it("echoes the request and namespace", function () {
        withExtensions(extensions, testEcho, ["standalone", "sharded"]);
    });

    it("is not registered when featureFlagExtensionCommands is disabled", function () {
        withExtensions(
            extensions,
            (conn) => {
                assert.commandFailedWithCode(
                    conn.getDB(jsTestName()).runCommand({testEcho: "coll"}),
                    ErrorCodes.CommandNotFound,
                );
            },
            [
                {
                    topology: "standalone",
                    mongodOptions: {setParameter: {featureFlagExtensionCommands: false}},
                },
            ],
        );
    });
});
