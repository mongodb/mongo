//
// Run 'listDatabases' while renaming a collection concurrently. See SERVER-34531.
//

(function() {
    "use strict";
    const dbName = "list_databases_rename";
    const collName = "collA";
    const repeatListDatabases = 20;
    const listDatabasesCmd = {"listDatabases": 1};

    // To be called from startParallelShell.
    function doRenames() {
        const dbName = "list_databases_rename";
        const collName = "collA";
        const repeatRename = 200;
        // Signal to the parent shell that the parallel shell has started.
        assert.writeOK(db.await_data.insert({_id: "signal parent shell"}));
        const otherName = "collB";
        let listRenameDB = db.getSiblingDB(dbName);
        for (let i = 0; i < repeatRename; i++) {
            // Rename the collection back and forth.
            assert.commandWorked(listRenameDB[collName].renameCollection(otherName));
            assert.commandWorked(listRenameDB[otherName].renameCollection(collName));
        }
        // Signal to the parent shell that the renames have completed.
        assert.writeOK(db.await_data.insert({_id: "rename has ended"}));
    }

    const conn = MongoRunner.runMongod({});
    assert.neq(null, conn, "mongod was unable to start up");
    jsTestLog("Create collection.");
    let listRenameDB = conn.getDB(dbName);
    listRenameDB.dropDatabase();
    assert.commandWorked(listRenameDB.runCommand({"create": collName}));

    let testDB = conn.getDB("test");
    testDB.dropDatabase();

    jsTestLog("Verify database exists.");
    let cmdRes = listRenameDB.adminCommand(listDatabasesCmd);
    assert.commandWorked(cmdRes, "expected " + tojson(listDatabasesCmd) + " to be successful.");
    assert(cmdRes.hasOwnProperty("databases"),
           "expected " + tojson(cmdRes) + " to have a databases property.");
    assert(cmdRes.databases.map(d => d.name).includes(dbName),
           "expected " + tojson(cmdRes) + " to include " + dbName);

    jsTestLog("Start parallel shell");
    let renameShell = startParallelShell(doRenames, conn.port);

    // Wait until we receive confirmation that the parallel shell has started.
    assert.soon(() => conn.getDB("test").await_data.findOne({_id: "signal parent shell"}) !== null);

    jsTestLog("Start listDatabases.");
    while (conn.getDB("test").await_data.findOne({_id: "rename has ended"}) == null) {
        for (let i = 0; i < repeatListDatabases; i++) {
            cmdRes = listRenameDB.adminCommand(listDatabasesCmd);
            assert.commandWorked(cmdRes,
                                 "expected " + tojson(listDatabasesCmd) + " to be successful.");
            // Database should always exist.
            assert(cmdRes.hasOwnProperty("databases"),
                   "expected " + tojson(cmdRes) + " to have a databases property.");
            assert(cmdRes.databases.map(d => d.name).includes(dbName),
                   "expected " + tojson(cmdRes) + " to include " + dbName);
        }
    }

    jsTestLog("Finished running listDatabases.");

    renameShell();
    MongoRunner.stopMongod(conn);

}());
