/**
 * Tests for the listDatabases command.
 */
(function() {
    "use strict";

    // Given the output from the listDatabases command, ensures that the total size reported is the
    // sum of the individual db sizes.
    function verifySizeSum(listDatabasesOut) {
        assert(listDatabasesOut.hasOwnProperty("databases"));
        const dbList = listDatabasesOut.databases;
        let sizeSum = 0;
        for (let i = 0; i < dbList.length; i++) {
            sizeSum += dbList[i].sizeOnDisk;
        }
        assert.eq(sizeSum, listDatabasesOut.totalSize);
    }

    // Make 4 test databases.
    db.getSiblingDB("jstest_list_databases_foo").coll.insert({});
    db.getSiblingDB("jstest_list_databases_bar").coll.insert({});
    db.getSiblingDB("jstest_list_databases_baz").coll.insert({});
    db.getSiblingDB("jstest_list_databases_zap").coll.insert({});

    let cmdRes = assert.commandWorked(
        db.adminCommand({listDatabases: 1, filter: {name: /jstest_list_databases/}}));
    assert.eq(4, cmdRes.databases.length);
    verifySizeSum(cmdRes);

    // Now only list databases starting with a particular prefix.
    cmdRes = assert.commandWorked(
        db.adminCommand({listDatabases: 1, filter: {name: /^jstest_list_databases_ba/}}));
    assert.eq(2, cmdRes.databases.length);
    verifySizeSum(cmdRes);

    // Now return only the admin database.
    cmdRes = assert.commandWorked(db.adminCommand({listDatabases: 1, filter: {name: "admin"}}));
    assert.eq(1, cmdRes.databases.length);
    verifySizeSum(cmdRes);
}());
