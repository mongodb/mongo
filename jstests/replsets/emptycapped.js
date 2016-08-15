// This tests the emptycapped command in a replica set.

(function() {
    "use strict";
    var rst = new ReplSetTest({nodes: 2});
    rst.startSet();
    rst.initiate();

    var primaryTestDB = rst.getPrimary().getDB('test');
    var primaryLocalDB = rst.getPrimary().getDB('local');
    var primaryAdminDB = rst.getPrimary().getDB('admin');
    var secondaryTestDB = rst.getSecondary().getDB('test');

    // Truncate a non-capped collection.
    assert.writeOK(primaryTestDB.noncapped.insert({x: 1}));
    assert.commandWorked(primaryTestDB.runCommand({emptycapped: 'noncapped'}));
    assert.eq(primaryTestDB.noncapped.find().itcount(),
              0,
              "Expected 0 documents to exist after emptying the collection");

    // Truncate a non-existent collection on a non-existent database.
    assert.commandWorked(rst.getPrimary().getDB('nonexistent').dropDatabase());
    assert.commandFailedWithCode(
        rst.getPrimary().getDB('nonexistent').runCommand({emptycapped: 'nonexistent'}), 13429);

    // Truncate a non-existent collection.
    primaryTestDB.nonexistent.drop();
    assert.commandFailedWithCode(primaryTestDB.runCommand({emptycapped: 'nonexistent'}), 28584);

    // Truncate a capped collection.
    assert.commandWorked(primaryTestDB.createCollection("capped", {capped: true, size: 4096}));
    assert.writeOK(primaryTestDB.capped.insert({}));
    assert.eq(
        primaryTestDB.capped.find().itcount(), 1, "Expected 1 document to exist after an insert");
    assert.commandWorked(primaryTestDB.runCommand({emptycapped: 'capped'}));
    assert.eq(primaryTestDB.capped.find().itcount(),
              0,
              "Expected 0 documents to exist after emptying the collection");

    // Truncate a capped collection on a secondary.
    assert.commandFailedWithCode(secondaryTestDB.runCommand({emptycapped: 'capped'}),
                                 ErrorCodes.NotMaster);

    // Truncate the oplog.
    assert.commandFailedWithCode(primaryLocalDB.runCommand({emptycapped: "oplog.rs"}),
                                 ErrorCodes.OplogOperationUnsupported);

    // Test system collections, which cannot be truncated except system.profile.

    // Truncate the local system.js collection.
    assert.writeOK(primaryTestDB.system.js.insert({_id: "mystring", value: "var root = this;"}));
    assert.commandFailedWithCode(primaryTestDB.runCommand({emptycapped: "system.js"}),
                                 ErrorCodes.IllegalOperation);

    // Truncate the system.profile collection.
    assert.commandWorked(
        primaryTestDB.createCollection("system.profile", {capped: true, size: 4096}));
    assert.commandWorked(primaryTestDB.runCommand({profile: 2}));
    assert.commandWorked(primaryTestDB.runCommand({emptycapped: "system.profile"}));
    assert.commandWorked(primaryTestDB.runCommand({profile: 0}));
    assert(primaryTestDB.system.profile.drop(), "Failed to drop the system.profile collection");

    // Truncate the local system.replset collection.
    assert.commandFailedWithCode(primaryLocalDB.runCommand({emptycapped: "system.replset"}),
                                 ErrorCodes.IllegalOperation);

    // Test user & role management system collections.
    assert.commandWorked(primaryAdminDB.runCommand({
        createRole: "all1",
        privileges: [{resource: {db: "", collection: ""}, actions: ["anyAction"]}],
        roles: []
    }));
    assert.commandWorked(primaryAdminDB.runCommand(
        {createUser: "root2", pwd: "pwd", roles: [{role: "root", db: "admin"}]}));

    // TODO: Test system.backup_users & system.new_users.

    // Truncate the admin system.roles collection.
    assert.commandFailedWithCode(primaryAdminDB.runCommand({emptycapped: "system.roles"}),
                                 ErrorCodes.IllegalOperation);

    // Truncate the admin system.users collection.
    assert.commandFailedWithCode(primaryAdminDB.runCommand({emptycapped: "system.users"}),
                                 ErrorCodes.IllegalOperation);

    // Truncate the admin system.version collection.
    assert.commandFailedWithCode(primaryAdminDB.runCommand({emptycapped: "system.version"}),
                                 ErrorCodes.IllegalOperation);

    // Truncate the local system.views collection.
    assert.commandWorked(primaryTestDB.runCommand(
        {create: "view1", viewOn: "collection", pipeline: [{$match: {}}]}));
    assert.commandFailedWithCode(primaryTestDB.runCommand({emptycapped: "system.views"}),
                                 ErrorCodes.IllegalOperation);
})();
