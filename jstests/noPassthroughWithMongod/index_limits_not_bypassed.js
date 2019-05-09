/**
 * Ensures that we cannot bypass the 64 index limit or the 1 text index limit per collection using
 * the 'createIndexes()' command to create multiple indexes in one request.
 */
(function() {
    "use strict";

    const collName = "index_limits_not_bypassed";
    const coll = db.getCollection(collName);
    coll.drop();

    // A single collection can have no more than 64 indexes. We'll create 62 indexes here to
    // have a total of 63 indexes (the _id index and the 62 about to be created).
    for (let index = 0; index < 62; index++) {
        let spec = {};
        spec[index] = 1;
        assert.commandWorked(coll.createIndex(spec));
    }

    let indexes = db.runCommand({listIndexes: collName});
    assert.eq(63, indexes.cursor.firstBatch.length);

    // Creating multiple indexes via 'createIndexes()' shouldn't bypass index limits.
    assert.commandFailedWithCode(coll.createIndexes([{x: 1}, {y: 1}]),
                                 ErrorCodes.CannotCreateIndex);

    assert.commandFailedWithCode(coll.dropIndex("x"), ErrorCodes.IndexNotFound);
    assert.commandFailedWithCode(coll.dropIndex("y"), ErrorCodes.IndexNotFound);

    // Try to create two text indexes at the same time using 'createIndexes()'. The limit for text
    // indexes is one per collection.
    assert.commandFailedWithCode(
        coll.createIndexes([{x: "text", weights: {x: 5}}, {y: "text", weights: {y: 10}}]),
        ErrorCodes.CannotCreateIndex);

    assert.commandFailedWithCode(coll.dropIndex("x"), ErrorCodes.IndexNotFound);
    assert.commandFailedWithCode(coll.dropIndex("y"), ErrorCodes.IndexNotFound);
}());
