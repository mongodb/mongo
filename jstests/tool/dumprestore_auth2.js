// dumprestore_auth2.js
// Tests that mongodump and mongorestore properly handle access control information
// Tests that the default auth roles of backup and restore work properly.

var dumpRestoreAuth2 = function(backup_role, restore_role) {

    t = new ToolTest("dumprestore_auth2", {auth: ""});

    coll = t.startDB("foo");
    admindb = coll.getDB().getSiblingDB("admin");

    // Create the relevant users and roles.
    admindb.createUser({user: "root", pwd: "pass", roles: ["root"]});
    admindb.auth("root", "pass");

    admindb.createUser({user: "backup", pwd: "pass", roles: [backup_role]});
    admindb.createUser({user: "restore", pwd: "pass", roles: [restore_role]});

    admindb.createRole({
        role: "customRole",
        privileges: [{
            resource: {db: "jstests_tool_dumprestore_auth2", collection: "foo"},
            actions: ["find"]
        }],
        roles: []
    });
    admindb.createUser({user: "test", pwd: "pass", roles: ["customRole"]});

    coll.insert({word: "tomato"});
    assert.eq(1, coll.count());

    assert.eq(4, admindb.system.users.count(), "setup users");
    assert.eq(2,
              admindb.system.users.getIndexes().length,
              "setup2: " + tojson(admindb.system.users.getIndexes()));
    assert.eq(1, admindb.system.roles.count(), "setup3");
    assert.eq(2, admindb.system.roles.getIndexes().length, "setup4");
    assert.eq(1, admindb.system.version.find({_id: "authSchema"}).count());
    var versionDoc = admindb.system.version.findOne({_id: "authSchema"});

    // Logout root user.
    admindb.logout();

    // Verify that the custom role works as expected.
    admindb.auth("test", "pass");
    assert.eq("tomato", coll.findOne().word);
    admindb.logout();

    // Dump the database.
    t.runTool("dump", "--out", t.ext, "--username", "backup", "--password", "pass");

    // Drop the relevant data in the database.
    admindb.auth("root", "pass");
    coll.getDB().dropDatabase();
    admindb.dropUser("backup");
    admindb.dropUser("test");
    admindb.dropRole("customRole");

    assert.eq(2, admindb.system.users.count(), "didn't drop backup and test users");
    assert.eq(0, admindb.system.roles.count(), "didn't drop roles");
    assert.eq(0, coll.count(), "didn't drop foo coll");

    // This test depends on W=0 to mask unique index violations.
    // This should be fixed once we implement TOOLS-341
    t.runTool("restore",
              "--dir",
              t.ext,
              "--username",
              "restore",
              "--password",
              "pass",
              "--writeConcern",
              "0");

    assert.soon("admindb.system.users.findOne()", "no data after restore");
    assert.eq(4, admindb.system.users.count(), "didn't restore users");
    assert.eq(2, admindb.system.users.getIndexes().length, "didn't restore user indexes");
    assert.eq(1, admindb.system.roles.find({role: 'customRole'}).count(), "didn't restore roles");
    assert.eq(2, admindb.system.roles.getIndexes().length, "didn't restore role indexes");

    admindb.logout();

    // Login as user with customRole to verify privileges are restored.
    admindb.auth("test", "pass");
    assert.eq("tomato", coll.findOne().word);
    admindb.logout();

    admindb.auth("root", "pass");
    admindb.createUser({user: "root2", pwd: "pass", roles: ["root"]});
    admindb.dropRole("customRole");
    admindb.createRole({role: "customRole2", roles: [], privileges: []});
    admindb.dropUser("root");
    admindb.logout();

    t.runTool("restore",
              "--dir",
              t.ext,
              "--username",
              "restore",
              "--password",
              "pass",
              "--drop",
              "--writeConcern",
              "0");

    admindb.auth("root", "pass");
    assert.soon("1 == admindb.system.users.find({user:'root'}).count()", "didn't restore users 2");
    assert.eq(0, admindb.system.users.find({user: 'root2'}).count(), "didn't drop users");
    assert.eq(0, admindb.system.roles.find({role: 'customRole2'}).count(), "didn't drop roles");
    assert.eq(1, admindb.system.roles.find({role: 'customRole'}).count(), "didn't restore roles");
    assert.eq(2, admindb.system.users.getIndexes().length, "didn't maintain user indexes");
    assert.eq(2, admindb.system.roles.getIndexes().length, "didn't maintain role indexes");
    assert.eq(
        1, admindb.system.version.find({_id: "authSchema"}).count(), "didn't restore version");
    assert.docEq(versionDoc,
                 admindb.system.version.findOne({_id: "authSchema"}),
                 "version doc wasn't restored properly");
    admindb.logout();

    t.stop();

};

// Tests that the default auth roles of backup and restore work properly.
dumpRestoreAuth2("backup", "restore");

// Tests that root has backup and restore privileges too.
dumpRestoreAuth2("root", "root");