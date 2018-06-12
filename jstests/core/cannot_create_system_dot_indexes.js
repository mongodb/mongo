// Tests that a user cannot create the 'system.indexes' collection.
// @tags: [requires_non_retryable_commands]
(function() {
    "use strict";

    // Cannot create system.indexes using the 'create' command.
    assert.commandFailedWithCode(db.createCollection("system.indexes"),
                                 ErrorCodes.InvalidNamespace);
    assert.eq(
        0, db.getCollectionInfos({name: "system.indexes"}).length, tojson(db.getCollectionInfos()));

    // Cannot create system.indexes using the 'renameCollection' command.
    db.coll.drop();
    assert.commandWorked(db.coll.insert({}));
    assert.commandFailed(db.coll.renameCollection("system.indexes"));
    assert.eq(
        0, db.getCollectionInfos({name: "system.indexes"}).length, tojson(db.getCollectionInfos()));

    // Cannot create system.indexes using the 'createIndexes' command.
    assert.commandFailedWithCode(db.system.indexes.createIndex({a: 1}),
                                 ErrorCodes.InvalidNamespace);
    assert.eq(
        0, db.getCollectionInfos({name: "system.indexes"}).length, tojson(db.getCollectionInfos()));

    // Cannot create system.indexes using the 'insert' command.
    assert.commandWorked(
        db.system.indexes.insert({ns: "test.coll", v: 2, key: {a: 1}, name: "a_1"}));
    assert.eq(
        0, db.getCollectionInfos({name: "system.indexes"}).length, tojson(db.getCollectionInfos()));

    // Cannot create system.indexes using the 'update' command with upsert=true.
    assert.commandFailedWithCode(
        db.system.indexes.update({ns: "test.coll", v: 2, key: {a: 1}, name: "a_1"},
                                 {$set: {name: "a_1"}},
                                 {upsert: true}),
        ErrorCodes.InvalidNamespace);
    assert.eq(
        0, db.getCollectionInfos({name: "system.indexes"}).length, tojson(db.getCollectionInfos()));

    // Cannot create system.indexes using the 'findAndModify' command with upsert=true.
    let error = assert.throws(() => {
        db.system.indexes.findAndModify({
            query: {ns: "test.coll", v: 2, key: {a: 1}, name: "a_1"},
            update: {$set: {name: "a_1"}},
            upsert: true
        });
    });
    assert.eq(error.code, ErrorCodes.InvalidNamespace);
    assert.eq(
        0, db.getCollectionInfos({name: "system.indexes"}).length, tojson(db.getCollectionInfos()));
}());
