// Tests that SERVER-11374 is fixed: specifically, that indexes cannot
// be created on fields that begin with '$' but are not part of DBRefs
// and that indexes cannot be created on field paths that contain empty
// fields.

var coll = db.jstests_indexp;

// Empty field checks.
assert.commandFailed(coll.ensureIndex({'a..b': 1}));
assert.commandFailed(coll.ensureIndex({'.a': 1}));
assert.commandFailed(coll.ensureIndex({'a.': 1}));
assert.commandFailed(coll.ensureIndex({'.': 1}));
assert.commandFailed(coll.ensureIndex({'': 1}));
assert.commandWorked(coll.ensureIndex({'a.b': 1}));

// '$'-prefixed field checks.
assert.commandFailed(coll.ensureIndex({'$a': 1}));
assert.commandFailed(coll.ensureIndex({'a.$b': 1}));
assert.commandFailed(coll.ensureIndex({'$db': 1}));
assert.commandWorked(coll.ensureIndex({'a$ap': 1}));   // $ in middle is ok
assert.commandWorked(coll.ensureIndex({'a.$id': 1}));  // $id/$db/$ref are execptions

coll.dropIndexes();
