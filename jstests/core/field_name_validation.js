/**
 * Test the behavior of field names containing special characters, including:
 * - dots
 * - $-prefixed
 * - reserved $-prefixed ($ref, $id, $db)
 * - regex
 *
 * contained in a top-level element, embedded element, and within _id.
 *
 * @tags: [
 *   assumes_unsharded_collection,
 * ]
 */
(function() {
"use strict";

const coll = db.field_name_validation;
coll.drop();

//
// Insert command field name validation.
//

// Test that dotted field names are allowed.
assert.commandWorked(coll.insert({"a.b": 1}));
assert.commandWorked(coll.insert({"_id.a": 1}));
assert.commandWorked(coll.insert({a: {"a.b": 1}}));
assert.commandWorked(coll.insert({_id: {"a.b": 1}}));

// Test that _id cannot be a regex.
assert.writeError(coll.insert({_id: /a/}));

// Test that _id cannot be an array.
assert.writeError(coll.insert({_id: [9]}));

// Test that $-prefixed field names are allowed in embedded objects.
assert.commandWorked(coll.insert({a: {$b: 1}}));
assert.eq(1, coll.find({"a.$b": 1}).itcount());

// An embedded _id field can be an object with an element that has a $-prefixed name.
assert.commandWorked(coll.insert({a: {_id: [9]}}));
assert.commandWorked(coll.insert({a: {_id: /a/}}));
assert.commandWorked(coll.insert({a: {_id: {$b: 1}}}));

// Test that $-prefixed field names are allowed generally.
assert.commandWorked(coll.insert({$a: 1}));
assert.commandWorked(coll.insert({valid: 1, $a: 1}));
assert.commandWorked(coll.insert({$a: {$b: 1}}));

// Test that reserved $-prefixed field names are allowed.
assert.commandWorked(coll.insert({$ref: 1}));
assert.commandWorked(coll.insert({$id: 1}));
assert.commandWorked(coll.insert({$db: 1}));

// Test that _id cannot be an object with an element that has a $-prefixed field name.
assert.writeErrorWithCode(coll.insert({_id: {$b: 1}}), ErrorCodes.DollarPrefixedFieldName);
assert.writeErrorWithCode(coll.insert({_id: {a: 1, $b: 1}}), ErrorCodes.DollarPrefixedFieldName);

// Test that inserting an object with a $-prefixed field name is properly validated.
assert.commandWorked(coll.insert({_id: 0, $valid: 1, "a": 1}));
assert.eq([{_id: 0, $valid: 1, "a": 1}], coll.find({_id: 0}).toArray());

// Test that previously reserved fieldnames may be inserted when the feature flag is enabled.
assert.commandWorked(coll.insert({_id: 1, $valid: 1, $id: 1}));
assert.commandWorked(coll.insert({_id: 2, $valid: 1, $db: 1}));
assert.commandWorked(coll.insert({_id: 3, $valid: 1, $ref: 1}));
assert.commandWorked(coll.insert({_id: 4, $valid: 1, $alsoValid: 1}));

// Valid, because _id.$gt is a field name, and not equivalent to {_id: {$gt: 4}}
assert.commandWorked(coll.insert({"_id.$gt": 4}));

//
// Update command field name validation.
//
coll.drop();

// Dotted fields are allowed in an update.
assert.commandWorked(coll.update({}, {"a.b": 1}, {upsert: true}));
assert.eq(0, coll.find({"a.b": 1}).itcount());
assert.eq(1, coll.find({}).itcount());

// Dotted fields represent paths in $set.
assert.commandWorked(coll.update({}, {$set: {"a.b": 1}}, {upsert: true}));
assert.eq(1, coll.find({"a.b": 1}).itcount());

// Dotted fields represent paths in the query object.
assert.commandWorked(coll.update({"a.b": 1}, {$set: {"a.b": 2}}));
assert.eq(1, coll.find({"a.b": 2}).itcount());
assert.eq(1, coll.find({a: {b: 2}}).itcount());

assert.commandWorked(coll.update({"a.b": 2}, {"a.b": 3}));
assert.eq(0, coll.find({"a.b": 3}).itcount());

// Upserting _id fields containing $-prefixed fields is not allowed.
assert.writeErrorWithCode(coll.update({"a.b": 1}, {_id: {$invalid: 1}}, {upsert: true}),
                          ErrorCodes.DollarPrefixedFieldName);
assert.writeErrorWithCode(coll.update({"a.b": 1}, {$set: {_id: {$invalid: 1}}}, {upsert: true}),
                          ErrorCodes.DollarPrefixedFieldName);
assert.writeErrorWithCode(coll.update({"a.b": 1}, {$set: {"_id.$gt": 1}}, {upsert: true}),
                          ErrorCodes.DollarPrefixedFieldName);
assert.writeErrorWithCode(
    coll.update({"a.b": 1}, {$setOnInsert: {_id: {$invalid: 1}}}, {upsert: true}),
    ErrorCodes.DollarPrefixedFieldName);
assert.writeErrorWithCode(
    coll.update({"a.b": 1}, {$setOnInsert: {"_id.$invalid": 1}}, {upsert: true}),
    ErrorCodes.DollarPrefixedFieldName);
assert.writeErrorWithCode(coll.update({"_id.$gt": 1}, {$set: {a: 1}}, {upsert: true}),
                          ErrorCodes.DollarPrefixedFieldName);

// Replacement-style updates can contain nested $-prefixed fields.
assert.commandWorked(coll.update({"a.b": 1}, {a: {$c: 1}}));
assert.commandWorked(coll.update({"a.b": 2}, {a: {$c: 1}}, {upsert: true}));
assert.commandWorked(coll.update({"a.b": 1}, {foo: {$c: 1, bar: {$d: 4}}}));

// Pipeline-style updates are allowed to contain $-prefixed fields.
assert.commandWorked(coll.update({"a.b": 1}, [{$replaceWith: {$literal: {$a: 1}}}]));
assert.commandWorked(
    coll.update({"a.b": 3}, [{$replaceWith: {$literal: {a: {$a: 1}}}}], {upsert: true}));
assert.commandWorked(coll.update({"a.b": 1}, [{$replaceWith: {$literal: {a: {$a: 1}}}}]));

// Pipeline-style updates are allowed to contain reserved $-prefixed fields.
assert.commandWorked(coll.update({"a.b": 1}, [{$replaceWith: {$literal: {$ref: 1}}}]));
assert.commandWorked(
    coll.update({"a.b": 4}, [{$replaceWith: {$literal: {$ref: 1}}}], {upsert: true}));
assert.commandWorked(
    coll.update({"a.b": 5}, [{$replaceWith: {$literal: {$id: 1}}}], {upsert: true}));
assert.commandWorked(
    coll.update({"a.b": 6}, [{$replaceWith: {$literal: {$db: 1}}}], {upsert: true}));

// $-prefixed field names are not allowed at the top-level in replacement-style updates.
assert.writeErrorWithCode(coll.update({"a.b": 1}, {$c: 1}), ErrorCodes.FailedToParse);

// Top-level reserved $-prefixed field names are not allowed in replacement-style updates.
assert.writeErrorWithCode(coll.update({"a.b": 1}, {$ref: 1}), ErrorCodes.FailedToParse);
assert.writeErrorWithCode(coll.update({"a.b": 1}, {$id: 1}), ErrorCodes.FailedToParse);
assert.writeErrorWithCode(coll.update({"a.b": 1}, {$db: 1}), ErrorCodes.FailedToParse);

// Nested reserved $-prefixed field names are not allowed in replacement-style updates.
assert.commandWorked(coll.update({"a.b": 1}, {a: {$ref: 1}}));
assert.commandWorked(coll.update({"a.b": 1}, {b: {$id: 1}}));
assert.commandWorked(coll.update({"a.b": 1}, {c: {$db: 1}}));

//
// FindAndModify field name validation.
//
coll.drop();

// Dotted fields are allowed in update object.
coll.findAndModify({query: {_id: 0}, update: {_id: 0, "a.b": 1}, upsert: true});
assert.eq([{_id: 0, "a.b": 1}], coll.find({_id: 0}).toArray());

// Dotted fields represent paths in $set.
coll.findAndModify({query: {_id: 1}, update: {$set: {_id: 1, "a.b": 1}}, upsert: true});
assert.eq([{_id: 1, a: {b: 1}}], coll.find({_id: 1}).toArray());

// Dotted fields represent paths in the query object.
coll.findAndModify({query: {_id: 0, "a.b": 1}, update: {"a.b": 2}});
assert.eq([{_id: 0, "a.b": 1}], coll.find({_id: 0}).toArray());

coll.findAndModify({query: {_id: 1, "a.b": 1}, update: {$set: {_id: 1, "a.b": 2}}});
assert.eq([{_id: 1, a: {b: 2}}], coll.find({_id: 1}).toArray());

// Dotted fields in a $literal-wrapped document can be updated in a pipeline-style update.
coll.findAndModify({query: {_id: 1}, update: [{$replaceWith: {$literal: {_id: 1, "a.b": 3}}}]});
assert.eq([{_id: 1, "a.b": 3}], coll.find({_id: 1}).toArray());

coll.findAndModify({query: {_id: 1}, update: [{$replaceWith: {$literal: {_id: 1, "a.b.c": 3}}}]});
assert.eq([{_id: 1, "a.b.c": 3}], coll.find({_id: 1}).toArray());

// Dotted fields without $literal cannot be updated in a pipeline-style update.
assert.throws(function() {
    coll.findAndModify({query: {_id: 1}, update: [{$replaceWith: {_id: 1, "a.b.": 2}}]});
});

assert.throws(function() {
    coll.findAndModify({query: {_id: 1}, update: [{$replaceWith: {_id: 1, "a.b.c": 3}}]});
});

// Top-level $-prefixed field names are not allowed in a replacement-style update.
assert.throws(function() {
    coll.findAndModify({query: {_id: 1}, update: {_id: 1, $invalid: 1}});
});
assert.throws(function() {
    coll.findAndModify({query: {_id: 1}, update: {$set: {_id: 1, $invalid: 1}}});
});

// Reserved $-prefixed field names are not allowed in a replacement-style update.
assert.throws(function() {
    coll.findAndModify({query: {_id: 1}, update: {_id: 1, $ref: 1}});
});
assert.throws(function() {
    coll.findAndModify({query: {_id: 1}, update: {_id: 1, $id: 1}});
});
assert.throws(function() {
    coll.findAndModify({query: {_id: 1}, update: {_id: 1, $db: 1}});
});

// Test that $-prefixed fields are allowed when they are not at the top-level.
coll.findAndModify({query: {_id: 2}, update: {_id: 2, out: {$in: 1, "x": 2}}, upsert: true});
assert([{_id: 2, out: {$in: 1, "x": 2}}], coll.find({_id: 2}).toArray());

// Test that $-prefixed fields are allowed when a pipeline is used.
coll.findAndModify({
    query: {_id: 2},
    update: [{$replaceWith: {$literal: {_id: 2, $in: 2, "x": 3}}}],
    upsert: true
});
assert([{_id: 2, $in: 2, "x": 3}], coll.find({_id: 2}).toArray());

// Reserved $-prefixed field names are allowed when they are nested.
coll.findAndModify({query: {_id: 1}, update: {_id: 1, a: {$ref: 1}}});
assert.eq([{_id: 1, a: {$ref: 1}}], coll.find({_id: 1}).toArray());

coll.findAndModify({query: {_id: 1}, update: {_id: 1, a: {$id: 1}}});
assert.eq([{_id: 1, a: {$id: 1}}], coll.find({_id: 1}).toArray());

coll.findAndModify({query: {_id: 1}, update: {_id: 1, a: {$db: 1}}});
assert.eq([{_id: 1, a: {$db: 1}}], coll.find({_id: 1}).toArray());

//
// Aggregation field name validation.
//
coll.drop();

assert.commandWorked(coll.insert({_id: {a: 1, b: 2}, "c.d": 3}));

// Dotted fields represent paths in an aggregation pipeline.
assert.eq(coll.aggregate([{$match: {"_id.a": 1}}, {$project: {"_id.b": 1}}]).toArray(),
          [{_id: {b: 2}}]);
assert.eq(coll.aggregate([{$match: {"c.d": 3}}, {$project: {"_id.b": 1}}]).toArray(), []);

assert.eq(coll.aggregate([{$project: {"_id.a": 1}}]).toArray(), [{_id: {a: 1}}]);
assert.eq(coll.aggregate([{$project: {"c.d": 1, _id: 0}}]).toArray(), [{}]);

assert.eq(coll.aggregate([
                  {$addFields: {"new.field": {$multiply: ["$c.d", "$_id.a"]}}},
                  {$project: {"new.field": 1, _id: 0}}
              ])
              .toArray(),
          [{new: {field: null}}]);

assert.eq(coll.aggregate([{$group: {_id: "$_id.a", e: {$sum: "$_id.b"}}}]).toArray(),
          [{_id: 1, e: 2}]);
assert.eq(coll.aggregate([{$group: {_id: "$_id.a", e: {$sum: "$c.d"}}}]).toArray(),
          [{_id: 1, e: 0}]);

// Accumulation statements cannot have a dotted field name.
assert.commandFailed(db.runCommand(
    {aggregate: coll.getName(), pipeline: [{$group: {_id: "$_id.a", "e.f": {$sum: "$_id.b"}}}]}));

// $-prefixed field names are not allowed in an aggregation pipeline.
assert.commandFailed(
    db.runCommand({aggregate: coll.getName(), pipeline: [{$match: {"$invalid": 1}}]}));

assert.commandFailed(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [{$project: {"_id.a": 1, "$newField": {$multiply: ["$_id.b", "$_id.a"]}}}]
}));

assert.commandFailed(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [{$addFields: {"_id.a": 1, "$newField": {$multiply: ["$_id.b", "$_id.a"]}}}]
}));

assert.commandFailed(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [{$group: {_id: "$_id.a", "$invalid": {$sum: "$_id.b"}}}]
}));
})();
