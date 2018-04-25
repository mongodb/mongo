// Verify custom roles still exist after noop collMod calls

(function() {
    'use strict';
    print("START auth-system-roles-collMod.js");
    var conn = MongoRunner.runMongod({});
    var db = conn.getDB("test");

    assert.commandWorked(db.runCommand(
        {createRole: "role1", roles: [{role: "readWrite", db: "test"}], privileges: []}));
    assert(db.runCommand({rolesInfo: "role1"}).roles[0].role === "role1");

    // RoleGraph not invalidated after empty collMod
    assert.commandWorked(db.adminCommand({collMod: "system.roles"}));
    assert(db.runCommand({rolesInfo: "role1"}).roles[0].role === "role1");

    // RoleGraph invalidated after non-empty collMod
    assert.commandWorked(db.adminCommand({collMod: "system.roles", validationLevel: "off"}));
    assert(db.runCommand({rolesInfo: "role1"}).roles.length === 0);

    print("SUCCESS auth-system-roles-collMod.js");
    MongoRunner.stopMongod(conn);
})();
