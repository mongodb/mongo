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

    it("stops a long-running command when its maxTimeMS expires", function () {
        // extInterruptibleEcho polls the host's interrupt check while sleeping, so the expired
        // deadline is observed inside the extension and surfaced as the operation's
        // MaxTimeMSExpired error.
        withExtensions(
            extensions,
            (conn) => {
                const db = conn.getDB(jsTestName());
                const res = db.runCommand({
                    extInterruptibleEcho: "coll",
                    sleepMs: 60 * 1000,
                    maxTimeMS: 100,
                });
                assert.commandFailedWithCode(res, ErrorCodes.MaxTimeMSExpired, {res});
            },
            ["standalone", "sharded"],
        );
    });

    it("stops a long-running command when it is killed", function () {
        // Runs extInterruptibleEcho in a parallel shell, kills it from here, and checks that the
        // extension observed the interrupt and failed with Interrupted.
        withExtensions(
            extensions,
            (conn) => {
                const db = conn.getDB(jsTestName());
                const awaitEcho = startParallelShell(
                    `const res = db.getSiblingDB(${tojson(db.getName())}).runCommand(
                         {extInterruptibleEcho: "coll", sleepMs: 60 * 1000});
                     assert.commandFailedWithCode(res, ErrorCodes.Interrupted);`,
                    conn.port,
                );

                // On a sharded cluster the command runs locally on the mongos, so query local ops
                // only; db.currentOp() would otherwise report shard operations.
                let opId;
                assert.soon(() => {
                    const inprog = db
                        .getSiblingDB("admin")
                        .aggregate([
                            {$currentOp: {localOps: true}},
                            {$match: {"command.extInterruptibleEcho": {$exists: true}}},
                        ])
                        .toArray();
                    if (inprog.length > 0) {
                        opId = inprog[0].opid;
                        return true;
                    }
                    return false;
                }, "extInterruptibleEcho never appeared in currentOp");

                db.killOp(opId);

                assert.eq(awaitEcho(), 0, "extInterruptibleEcho was not interrupted by killOp");
            },
            ["standalone", "sharded"],
        );
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
