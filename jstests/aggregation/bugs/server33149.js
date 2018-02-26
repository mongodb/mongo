(function() {
    "use strict";

    // For arrayEq.
    load("jstests/aggregation/extras/utils.js");

    let indexDB = db.getSiblingDB("create_indexess_db");
    assert.commandWorked(indexDB.dropDatabase());

    // Create an index with key: {x : 1}, name : "x_1
    assert.commandWorked(indexDB.runCommand({ createIndexes : "test", indexes : [ { key : { x : 1 }, name : "x_1" } ] }));

    // Create a duplicate index will not result an error
    assert.commandWorked(indexDB.runCommand({ createIndexes : "test", indexes : [ { key : { x : 1 }, name : "x_1"} ] }), ErrorCodes.IndexAlreadyExists, "Identical index already exists ");

    // Create an index that has same key pattern but with different name will fail
     assert.commandFailedWithCode(indexDB.runCommand({ createIndexes : "test", indexes : [ { key : { x : 1 }, name : "x_2"} ] }), ErrorCodes.CannotCreateIndex, "Index with the same key pattern already exists with a different name");

    assert.ccommandFailedWithCode(indexDB.runCommand({ createIndexes : "test", indexes: [ { key : {x : 2}, name : 'x_2' } ] }), ErrorCodes.IndexKeySpecsConflict, "Index with the same name, different key pattern already exists ");
})();
