// Tests that no-op createIndex commands do not block behind other operations holding an IX lock.
(function() {
    "use strict";

    const dbName = 'noop_createIndexes_not_blocked';
    const collName = 'test';

    let conn = MongoRunner.runMongod({dbpath: MongoRunner.dataPath + dbName});
    let testDB = conn.getDB(dbName);

    testDB.runCommand({drop: collName, writeConcern: {w: "majority"}});

    const createIndexesCommand = {createIndexes: collName, indexes: [{key: {a: 1}, name: "a_1"}]};
    assert.commandWorked(testDB.runCommand(createIndexesCommand));

    assert.commandWorked(
        testDB.adminCommand({configureFailPoint: 'hangAfterStartingIndexBuild', mode: 'alwaysOn'}));

    // Hang a BG index build and cause it to hold an IX lock on the database.
    let awaitBgIndexBuild = startParallelShell(function() {
        let testDB = db.getSiblingDB('noop_createIndexes_not_blocked');
        assert.commandWorked(testDB.runCommand({
            createIndexes: 'test',
            indexes: [{name: 'noop_1', key: {noop: 1}, background: true}],
            maxTimeMS: 5 * 60 * 1000
        }));
    }, conn.port);

    // This should not block because an identical index exists.
    let res = testDB.runCommand(createIndexesCommand);
    assert.commandWorked(res);
    assert.eq(res.numIndexesBefore, res.numIndexesAfter);

    // This should not block but return an error because the index exists with different options.
    res = testDB.runCommand({
        createIndexes: collName,
        indexes: [{key: {a: 1}, name: "unique_a_1", unique: true}],
    });
    assert.commandFailedWithCode(res, ErrorCodes.IndexOptionsConflict);

    // This should block and time out because the index does not already exist.
    res = testDB.runCommand(
        {createIndexes: collName, indexes: [{key: {b: 1}, name: "b_1"}], maxTimeMS: 500});
    assert.commandFailedWithCode(res, ErrorCodes.ExceededTimeLimit);

    // This should block and time out because one of the indexes does not already exist.
    res = testDB.runCommand({
        createIndexes: collName,
        indexes: [{key: {a: 1}, name: "a_1"}, {key: {b: 1}, name: "b_1"}],
        maxTimeMS: 500
    });
    assert.commandFailedWithCode(res, ErrorCodes.ExceededTimeLimit);

    assert.commandWorked(
        testDB.adminCommand({configureFailPoint: 'hangAfterStartingIndexBuild', mode: 'off'}));

    awaitBgIndexBuild();

    MongoRunner.stopMongod(conn);
}());
