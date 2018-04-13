// Tests that it is illegal to write to system collections within a transaction.
// @tags: [uses_transactions]
(function() {
    "use strict";

    const session = db.getMongo().startSession({causalConsistency: false});
    const testDB = session.getDatabase("test");

    // We can write to system.js, but we can't drop it. Given that, all reads and writes are on
    // fields that are not the _id to avoid duplicate key errors if the test is run on repeat.
    const systemColl = testDB.getCollection("system.js");

    // Ensure that a collection exists with at least one document.
    assert.commandWorked(systemColl.insert({name: 0}, {writeConcern: {w: "majority"}}));

    session.startTransaction({readConcern: {level: "snapshot"}});
    let error = assert.throws(() => systemColl.findAndModify({query: {}, update: {}}));
    assert.commandFailedWithCode(error, 50781);
    assert.throws(() => session.abortTransaction());

    session.startTransaction({readConcern: {level: "snapshot"}});
    error = assert.throws(() => systemColl.findAndModify({query: {}, remove: true}));
    assert.commandFailedWithCode(error, 50781);
    assert.throws(() => session.abortTransaction());

    session.startTransaction({readConcern: {level: "snapshot"}});
    assert.commandFailedWithCode(systemColl.insert({name: "new"}), 50784);
    assert.throws(() => session.abortTransaction());

    session.startTransaction({readConcern: {level: "snapshot"}});
    assert.commandFailedWithCode(systemColl.update({name: 0}, {$set: {name: "jungsoo"}}), 50783);
    assert.throws(() => session.abortTransaction());

    session.startTransaction({readConcern: {level: "snapshot"}});
    assert.commandFailedWithCode(
        systemColl.update({name: "nonexistent"}, {$set: {name: "jungsoo"}}, {upsert: true}), 50783);
    assert.throws(() => session.abortTransaction());

    session.startTransaction({readConcern: {level: "snapshot"}});
    assert.commandFailedWithCode(systemColl.remove({name: 0}), 50782);
    assert.throws(() => session.abortTransaction());
}());
