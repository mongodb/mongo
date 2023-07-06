/**
 * SERVER-75880 Test that _id cannot be an object with a deep nested element that has a $-prefixed
 * field name.
 *
 * @tags: [
 *   assumes_unsharded_collection,
 *   requires_fcv_71,
 * ]
 */
const coll = db.field_name_validation;
coll.drop();

// Insert command field name validation
assert.writeErrorWithCode(coll.insert({_id: {a: {$b: 1}}, x: 1}),
                          ErrorCodes.DollarPrefixedFieldName);

// Update commands with $set
coll.drop();
assert.commandWorked(coll.insert({_id: 0, a: 1}));
assert.writeErrorWithCode(coll.update({"_id.a.$b": 1}, {$set: {x: 1}}, {upsert: true}),
                          ErrorCodes.DollarPrefixedFieldName);
assert.writeErrorWithCode(coll.update({x: 1}, {$set: {_id: {a: {$b: 1}}}}, {upsert: true}),
                          ErrorCodes.DollarPrefixedFieldName);
assert.writeErrorWithCode(coll.update({a: 1}, {$set: {_id: {a: {$b: 1}}}}, {upsert: true}),
                          [ErrorCodes.DollarPrefixedFieldName, ErrorCodes.ImmutableField]);

// Replacement-style updates
coll.drop();
assert.commandWorked(coll.insert({_id: 0, a: 1}));
assert.writeErrorWithCode(coll.update({_id: 0}, {_id: {a: {$b: 1}}}),
                          ErrorCodes.DollarPrefixedFieldName);
assert.writeErrorWithCode(coll.update({"_id.a.$b": 1}, {_id: {a: {$b: 1}}}, {upsert: true}),
                          ErrorCodes.NotExactValueField);
assert.writeErrorWithCode(coll.update({_id: {a: {$b: 1}}}, {_id: {a: {$b: 1}}}, {upsert: true}),
                          ErrorCodes.DollarPrefixedFieldName);

// Pipeline-style updates with $replaceWith
coll.drop();
assert.commandWorked(coll.insert({_id: 0, a: 1}));
assert.writeErrorWithCode(coll.update({_id: 0}, [{$replaceWith: {$literal: {_id: {a: {$b: 1}}}}}]),
                          ErrorCodes.DollarPrefixedFieldName);
assert.writeErrorWithCode(
    coll.update({"_id.a.$b": 1}, [{$replaceWith: {$literal: {a: {$a: 1}}}}], {upsert: true}),
    ErrorCodes.DollarPrefixedFieldName);

// FindAndModify field name validation
coll.drop();
assert.commandWorked(coll.insert({_id: 0, a: 1}));
assert.throwsWithCode(() => {
    coll.findAndModify({query: {_id: 0}, update: {_id: {a: {$b: 1}}}});
}, ErrorCodes.DollarPrefixedFieldName);
assert.throwsWithCode(() => {
    coll.findAndModify({query: {"_id.a.$b": 1}, update: {_id: {a: {$b: 1}}}, upsert: true});
}, ErrorCodes.NotExactValueField);
assert.throwsWithCode(() => {
    coll.findAndModify({query: {_id: {a: {$b: 1}}}, update: {_id: {a: {$b: 1}}}, upsert: true});
}, ErrorCodes.DollarPrefixedFieldName);