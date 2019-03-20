(function() {
    "use strict";

    const indexDB = db.getSiblingDB("create_indexes_db");
    assert.commandWorked(indexDB.dropDatabase());

    assert.commandWorked(
        indexDB.runCommand({createIndexes: "test", indexes: [{key: {x: 1}, name: "x_1"}]}));

    // Testing that duplicate index cannot be created, but it will not result an error.
    assert.commandWorked(
        indexDB.runCommand({createIndexes: "test", indexes: [{key: {x: 1}, name: "x_1"}]}));

    // Testing the creation of an index that has same key pattern but with a different name will fail.
    assert.commandFailedWithCode(
        indexDB.runCommand({createIndexes: "test", indexes: [{key: {x: 1}, name: "x_2"}]}),
        ErrorCodes.CannotCreateIndex,
        "Index with the same option but a different name will not be created.");

    // Testing that index with different option and same name cannot be created, but it will not result an error.
    assert.commandWorked(
        indexDB.runCommand({createIndexes: "test", indexes: [{key: {x: 2}, name: 'x_2'}]}));
})();
