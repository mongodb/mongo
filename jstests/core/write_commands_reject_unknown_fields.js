// @tags: [requires_non_retryable_writes]

// SERVER-23129 Write commands should reject unknown fields. This is run in passthrough tests to
// ensure that both mongos and mongod reject these commands.
(function() {
    'use strict';

    var coll = db.write_commands_reject_unknown_fields;

    // All commands must reject fields at the top-level.
    assert.commandFailed(coll.runCommand('insert', {documents: [{}], asdf: true}));
    assert.commandFailed(
        coll.runCommand('update', {updates: [{q: {}, u: {$inc: {a: 1}}}], asdf: true}));
    assert.commandFailed(coll.runCommand('delete', {deletes: [{q: {}, limit: 0}], asdf: true}));

    // The inner objects in update and delete must also reject unknown fields. Insert isn't included
    // because its inner objects are the raw objects to insert and can have any fields.
    assert.commandFailed(
        coll.runCommand('update', {updates: [{q: {}, u: {$inc: {a: 1}}, asdf: true}]}));
    assert.commandFailed(coll.runCommand('delete', {deletes: [{q: {}, limit: 0, asdf: true}]}));
}());
