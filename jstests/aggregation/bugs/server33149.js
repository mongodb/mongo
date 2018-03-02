(function() {
    "use strict";

    // For arrayEq.
    load("jstests/aggregation/extras/utils.js");

    const indexDB = db.getSiblingDB("create_indexess_db");
    assert.commandWorked(indexDB.dropDatabase());

    // Create an index with key: {x : 1}, name : "x_1
    assert.commandWorked(
        indexDB.runCommand({createIndexes: "test", indexes: [{key: {x: 1}, name: "x_1"}]}));

    // Duplicate index cannot be created
    // But it will not result an error
    assert.commandWorked(
        indexDB.runCommand({createIndexes: "test", indexes: [{key: {x: 1}, name: "x_1"}]}));

    // Create an index that has same key pattern but with a different name will fail
    assert.commandFailedWithCode(
        indexDB.runCommand({createIndexes: "test", indexes: [{key: {x: 1}, name: "x_2"}]}),
        ErrorCodes.CannotCreateIndex,
        "Index with the same option but a different name will not be created.");

    // Index with different option and same name cannot be created
    // But it will not result an error
    assert.commandWorked(
        indexDB.runCommand({createIndexes: "test", indexes: [{key: {x: 2}, name: 'x_2'}]}));
})();
