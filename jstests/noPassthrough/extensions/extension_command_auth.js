/**
 * Tests that the host enforces the privileges an extension command declares before running it.
 * Uses the mock_commands extension: extRequiresFind requires the find action on its target
 * namespace, extRequiresFindDb requires it on the whole database, and testEcho declares no required
 * privileges.
 *
 * @tags: [featureFlagExtensionsAPI, featureFlagExtensionCommands]
 */
import {describe, it} from "jstests/libs/mochalite.js";
import {
    checkPlatformCompatibleWithExtensions,
    withExtensions,
} from "jstests/noPassthrough/libs/extension_helpers.js";

checkPlatformCompatibleWithExtensions();

const extensions = {"libmock_commands_extension.so": {}};
const collName = "coll";
const otherCollName = "otherColl";

function runTest(conn) {
    const admin = conn.getDB("admin");
    admin.createUser({user: "adminUser", pwd: "pwd", roles: ["root"]});
    assert(admin.auth("adminUser", "pwd"));

    const db = conn.getDB(jsTestName());
    const findOn = (coll) => [{resource: {db: db.getName(), collection: coll}, actions: ["find"]}];
    db.createRole({role: "canFind", privileges: findOn(collName), roles: []});
    db.createRole({role: "canFindOtherColl", privileges: findOn(otherCollName), roles: []});
    db.createRole({role: "canFindDb", privileges: findOn(""), roles: []});
    db.createUser({user: "findUser", pwd: "pwd", roles: ["canFind"]});
    db.createUser({user: "findDbUser", pwd: "pwd", roles: ["canFindDb"]});
    db.createUser({user: "otherCollUser", pwd: "pwd", roles: ["canFindOtherColl"]});
    db.createUser({user: "noPrivilegesUser", pwd: "pwd", roles: []});
    admin.logout();

    function asUser(user, fn) {
        assert(db.auth(user, "pwd"));
        try {
            fn();
        } finally {
            db.logout();
        }
    }

    // A user who holds the declared action on the target namespace can run the command.
    asUser("findUser", () => {
        assert.commandWorked(db.runCommand({extRequiresFind: collName}));
    });

    // A user without the declared action is rejected, but can still run a command that declares no
    // required privileges.
    asUser("noPrivilegesUser", () => {
        assert.commandFailedWithCode(
            db.runCommand({extRequiresFind: collName}),
            ErrorCodes.Unauthorized,
        );
        assert.commandWorked(db.runCommand({testEcho: collName}));
    });

    // Holding the declared action on a different namespace does not grant access.
    asUser("otherCollUser", () => {
        assert.commandFailedWithCode(
            db.runCommand({extRequiresFind: collName}),
            ErrorCodes.Unauthorized,
        );
        assert.commandWorked(db.runCommand({extRequiresFind: otherCollName}));
    });

    // A command that names no collection is checked against its database.
    asUser("findUser", () => {
        assert.commandFailedWithCode(db.runCommand({extRequiresFind: 1}), ErrorCodes.Unauthorized);
    });
    asUser("findDbUser", () => {
        assert.commandWorked(db.runCommand({extRequiresFind: 1}));
        assert.commandWorked(db.runCommand({extRequiresFind: collName}));
    });

    // A database-scoped privilege is enforced against the database, not the collection named in
    // the request: a user holding the action on only one collection cannot run the command.
    asUser("findUser", () => {
        assert.commandFailedWithCode(
            db.runCommand({extRequiresFindDb: collName}),
            ErrorCodes.Unauthorized,
        );
    });
    asUser("findDbUser", () => {
        assert.commandWorked(db.runCommand({extRequiresFindDb: collName}));
    });

    // An unauthenticated client cannot run any of the commands.
    assert.commandFailedWithCode(
        db.runCommand({extRequiresFind: collName}),
        ErrorCodes.Unauthorized,
    );
    assert.commandFailedWithCode(
        db.runCommand({extRequiresFindDb: collName}),
        ErrorCodes.Unauthorized,
    );
    assert.commandFailedWithCode(db.runCommand({testEcho: collName}), ErrorCodes.Unauthorized);
}

// Standalone and sharded need different configuration setups to enable auth, so they run
// separately.
describe("extension command authorization", function () {
    it("enforces declared privileges on a standalone", function () {
        withExtensions(extensions, runTest, ["standalone"], {}, {auth: ""});
    });

    it("enforces declared privileges on a sharded cluster", function () {
        withExtensions(extensions, runTest, ["sharded"], {other: {keyFile: "jstests/libs/key1"}});
    });
});
