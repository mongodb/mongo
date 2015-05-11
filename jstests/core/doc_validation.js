// Test basic inserts and updates with document validation.
(function() {
    "use strict";

    function assertFailsValidation(res) {
        var DocumentValidationFailure = 121;
        assert.writeError(res);
        assert.eq(res.getWriteError().code, DocumentValidationFailure);
    }

    var array = [];
    for (var i = 0; i < 2048; i++) {
        array.push({arbitrary: i});
    }

    var collName = "doc_validation";
    var coll = db[collName];
    coll.drop();

    // Create collection with document validator.
    assert.commandWorked(db.createCollection(collName, {validator: {a: {$exists: true}}}));

    // Insert and upsert documents that will pass validation.
    assert.writeOK(coll.insert({_id: 'valid1', a: 1}));
    assert.writeOK(coll.update({_id: 'valid2'}, {_id: 'valid2', a: 2}, {upsert: true}));

    // Insert and upsert documents that will not pass validation.
    assertFailsValidation(coll.insert({_id: 'invalid3', b: 1}));
    assertFailsValidation(coll.update({_id: 'invalid4'}, {_id: 'invalid4', b: 2}, {upsert: true}));

    // Remove document that passed validation.
    assert.writeOK(coll.remove({_id: 'valid1'}));

    // Drop will assert on failure.
    coll.drop();


    // Check that we can only update documents that pass validation.

    // Set up valid and invalid docs then set validator.
    assert.writeOK(coll.insert({_id: 'valid1', a: 1}));
    assert.writeOK(coll.insert({_id: 'invalid2', b: 1}));
    assert.commandWorked(db.runCommand({"collMod": collName,
                                        "validator" : {a: {$exists: true}}}));

    // Updates affecting fields not included in validator document
    // on a conforming document.

    // Add new field.
    assert.writeOK(coll.update({_id: 'valid1'}, {$set: {z: 1}}));
    // In place update.
    assert.writeOK(coll.update({_id: 'valid1'}, {$inc: {z: 1}}));
    // Out of place update.
    assert.writeOK(coll.update({_id: 'valid1'}, {$set: {z: array}}));
    // No-op update.
    assert.writeOK(coll.update({_id: 'valid1'}, {a: 1}));

    // Verify those updates will fail on non-conforming document.
    assertFailsValidation(coll.update({_id: 'invalid2'}, {$set: {z: 1}}));
    assertFailsValidation(coll.update({_id: 'invalid2'}, {$inc: {z: 1}}));
    assertFailsValidation(coll.update({_id: 'invalid2'}, {$set: {z: array}}));

    // A no-op update of an invalid doc will succeed.
    assert.writeOK(coll.update({_id: 'invalid2'}, {$set: {b: 1}}));

    coll.drop();


    // Verify can't make a conforming doc fail validation,
    // but can update non-conforming doc to pass validation.
    assert.writeOK(coll.insert({_id: 'valid1', a: 1}));
    assert.writeOK(coll.insert({_id: 'invalid2', b: 1}));
    assert.commandWorked(db.runCommand({"collMod": collName,
                                        "validator" : {a: {$exists: true}}}));

    assertFailsValidation(coll.update({_id: 'valid1'}, {$unset: {a: 1}}));
    assert.writeOK(coll.update({_id: 'invalid2'}, {$set: {a: 1}}));

    // Modify collection to remove validator statement
    assert.commandWorked(db.runCommand({"collMod": collName, "validator" : {}}));

    // Verify no validation applied to updates.
    assert.writeOK(coll.update({_id: 'valid1'}, {$set: {z: 1}}));
    assert.writeOK(coll.update({_id: 'invalid2'}, {$set: {z: 1}}));
    assert.writeOK(coll.update({_id: 'valid1'}, {$unset: {a: 1}}));
    assert.writeOK(coll.update({_id: 'invalid2'}, {$set: {a: 1}}));
    coll.drop();

})();
