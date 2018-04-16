// Tests that it is illegal to write to an unreplicated collection within a transaction.
// @tags: [uses_transactions]
(function() {
    "use strict";

    const session = db.getMongo().startSession({causalConsistency: false});
    const localDB = session.getDatabase("local");
    const unreplicatedColl = localDB.getCollection("no_writes_to_unreplicated_collection");

    const reply = unreplicatedColl.runCommand("drop", {writeConcern: {w: "majority"}});
    if (reply.ok !== 1) {
        assert.commandFailedWithCode(reply, ErrorCodes.NamespaceNotFound);
    }

    // Ensure that a collection exists with at least one document.
    assert.commandWorked(unreplicatedColl.insert({_id: 0}, {writeConcern: {w: "majority"}}));

    session.startTransaction({readConcern: {level: "snapshot"}});
    let error = assert.throws(() => unreplicatedColl.findAndModify({query: {}, update: {}}));
    assert.commandFailedWithCode(error, 50777);
    assert.throws(() => session.abortTransaction());

    session.startTransaction({readConcern: {level: "snapshot"}});
    error = assert.throws(() => unreplicatedColl.findAndModify({query: {}, remove: true}));
    assert.commandFailedWithCode(error, 50777);
    assert.throws(() => session.abortTransaction());

    session.startTransaction({readConcern: {level: "snapshot"}});
    assert.commandFailedWithCode(unreplicatedColl.insert({_id: "new"}), 50790);
    assert.throws(() => session.abortTransaction());

    session.startTransaction({readConcern: {level: "snapshot"}});
    assert.commandFailedWithCode(unreplicatedColl.update({_id: 0}, {$set: {name: "jungsoo"}}),
                                 50790);
    assert.throws(() => session.abortTransaction());

    session.startTransaction({readConcern: {level: "snapshot"}});
    assert.commandFailedWithCode(
        unreplicatedColl.update({_id: "nonexistent"}, {$set: {name: "jungsoo"}}, {upsert: true}),
        50790);
    assert.throws(() => session.abortTransaction());

    session.startTransaction({readConcern: {level: "snapshot"}});
    assert.commandFailedWithCode(unreplicatedColl.remove({_id: 0}), 50790);
    assert.throws(() => session.abortTransaction());
}());
