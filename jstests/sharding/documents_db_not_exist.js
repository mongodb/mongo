/**
 * Tests that $documents stage continues even when the database does not exist
 * @tags: [requires_fcv_62, multiversion_incompatible]
 *
 */

(function() {
"use strict";

let st = new ShardingTest({shards: 3});

function listDatabases(options) {
    return assert.commandWorked(st.s.adminCommand(Object.assign({listDatabases: 1}, options)))
        .databases;
}

function createAndDropDatabase(dbName) {
    // Create the database.
    let db = st.s.getDB(dbName);
    assert.commandWorked(db.foo.insert({}));
    // Confirms the database exists.
    assert.eq(1, listDatabases({nameOnly: true, filter: {name: dbName}}).length);
    // Drop the database
    assert.commandWorked(db.dropDatabase());
    // Confirm the database is dropped.
    assert.eq(0, listDatabases({nameOnly: true, filter: {name: dbName}}).length);
    return db;
}

// $documents stage evaluates to an array of objects.
let db = createAndDropDatabase("test");
let documents = [];
for (let i = 0; i < 50; i++) {
    documents.push({_id: i});
}
let result = db.aggregate([{$documents: documents}]);
assert(result.toArray().length == 50);

//$documents stage evaluates to an array of objects in a pipeline
db = createAndDropDatabase("test2");
result = db.aggregate([
    {$documents: [{_id: 1, size: "medium"}, {_id: 2, size: "large"}]},
    {$match: {size: "medium"}}
]);
assert(result.toArray().length == 1);

st.stop();
})();
