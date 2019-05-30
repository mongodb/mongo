/*
 * Similar to rename.js however this is requires profiling in order to assert that the failpoint
 * operated as it should which didn't work in rename7.js per SERVER-36717.
 */
// @tags: [requires_profiling]
(function() {
    // Set up namespaces a and b.
    var admin = db.getMongo().getDB("admin");
    var db_a = db.getMongo().getDB("db_a");
    var db_b = db.getMongo().getDB("db_b");

    var a = db_a.rename7;
    var b = db_b.rename7;

    // Ensure that the databases are created
    db_a.coll.insert({});
    db_b.coll.insert({});

    a.drop();
    b.drop();

    // Put some documents and indexes in a.
    a.save({a: 1});
    a.save({a: 2});
    a.save({a: 3});
    a.ensureIndex({a: 1});
    a.ensureIndex({b: 1});

    assert.commandWorked(admin.runCommand({renameCollection: "db_a.rename7", to: "db_b.rename7"}));

    assert.eq(0, a.find().count());
    assert(db_a.getCollectionNames().indexOf("rename7") < 0);

    assert.eq(3, b.find().count());
    assert(db_b.getCollectionNames().indexOf("rename7") >= 0);

    a.drop();
    b.drop();

    // Test that the dropTarget option works when renaming across databases.
    a.save({});
    b.save({});
    assert.commandFailed(admin.runCommand({renameCollection: "db_a.rename7", to: "db_b.rename7"}));

    // Ensure that a WCE during renaming doesn't cause a failure.
    assert.commandWorked(db_a.setProfilingLevel(2));  // So we can check WCE happens.
    assert.commandWorked(db_a.adminCommand(
        {"configureFailPoint": 'writeConflictInRenameCollCopyToTmp', "mode": {times: 1}}));
    assert.commandWorked(
        admin.runCommand({renameCollection: "db_a.rename7", to: "db_b.rename7", dropTarget: true}));
    assert.gte(db_a.system.profile.findOne().writeConflicts, 1);  // Make sure that our WCE happened
    assert.commandWorked(db_a.setProfilingLevel(0));
    a.drop();
    b.drop();
})();
