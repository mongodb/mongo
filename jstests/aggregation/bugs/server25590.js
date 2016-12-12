// Test that an aggregate command where the "pipeline" field has the wrong type fails with a
// TypeMismatch error.
(function() {
    "use strict";

    const coll = db.server25590;
    coll.drop();

    assert.writeOK(coll.insert({}));

    assert.commandFailed(db.runCommand({aggregate: coll.getName(), pipeline: 1}),
                         ErrorCodes.TypeMismatch);
    assert.commandFailed(db.runCommand({aggregate: coll.getName(), pipeline: {}}),
                         ErrorCodes.TypeMismatch);
    assert.commandFailed(db.runCommand({aggregate: coll.getName(), pipeline: [1, 2]}),
                         ErrorCodes.TypeMismatch);
    assert.commandFailed(db.runCommand({aggregate: coll.getName(), pipeline: [1, null]}),
                         ErrorCodes.TypeMismatch);
})();
