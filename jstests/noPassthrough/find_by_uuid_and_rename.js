//
// Run 'find' by UUID while renaming a collection concurrently. See SERVER-34615.
//

(function() {
    "use strict";
    const dbName = "do_concurrent_rename";
    const collName = "collA";
    const otherName = "collB";
    const repeatFind = 100;
    load("jstests/noPassthrough/libs/concurrent_rename.js");
    load("jstests/libs/parallel_shell_helpers.js");

    const conn = MongoRunner.runMongod({});
    assert.neq(null, conn, "mongod was unable to start up");
    jsTestLog("Create collection.");
    let findRenameDB = conn.getDB(dbName);
    findRenameDB.dropDatabase();
    assert.commandWorked(findRenameDB.runCommand({"create": collName}));
    assert.commandWorked(
        findRenameDB.runCommand({insert: collName, documents: [{fooField: 'FOO'}]}));

    let infos = findRenameDB.getCollectionInfos();
    let uuid = infos[0].info.uuid;
    const findCmd = {"find": uuid};

    // Assert 'find' command by UUID works.
    assert.commandWorked(findRenameDB.runCommand(findCmd));

    jsTestLog("Start parallel shell for renames.");
    let renameShell =
        startParallelShell(funWithArgs(doRenames, dbName, collName, otherName), conn.port);

    // Wait until we receive confirmation that the parallel shell has started.
    assert.soon(() => conn.getDB("test").await_data.findOne({_id: "signal parent shell"}) !== null,
                "Expected parallel shell to insert a document.");

    jsTestLog("Start 'find' commands.");
    while (conn.getDB("test").await_data.findOne({_id: "rename has ended"}) == null) {
        for (let i = 0; i < repeatFind; i++) {
            let res = findRenameDB.runCommand(findCmd);
            assert.commandWorked(res, "could not run " + tojson(findCmd));
            let cursor = new DBCommandCursor(findRenameDB, res);
            let errMsg = "expected more data from command " + tojson(findCmd) + ", with result " +
                tojson(res);
            assert(cursor.hasNext(), errMsg);
            let doc = cursor.next();
            assert.eq(doc.fooField, "FOO");
            assert(!cursor.hasNext(),
                   "expected to have exhausted cursor for results " + tojson(res));
        }
    }
    renameShell();
    MongoRunner.stopMongod(conn);

}());
