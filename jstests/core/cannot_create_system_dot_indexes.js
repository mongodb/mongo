// Tests that a user cannot create the 'system.indexes' collection.
// @tags: [requires_non_retryable_commands]
(function() {
    "use strict";

    // Cannot create system.indexes using the 'create' command.
    assert.commandFailedWithCode(db.createCollection("system.indexes"),
                                 ErrorCodes.InvalidNamespace);

    // Cannot create system.indexes using the 'renameCollection' command.
    db.coll.drop();
    assert.commandWorked(db.coll.insert({}));
    assert.commandFailed(db.coll.renameCollection("system.indexes"));

    // Cannot create system.indexes using the 'createIndexes' command.
    assert.commandFailedWithCode(db.system.indexes.createIndex({a: 1}),
                                 ErrorCodes.InvalidNamespace);
    assert.eq(
        0, db.getCollectionInfos({name: "system.indexes"}).length, tojson(db.getCollectionInfos()));
}());
