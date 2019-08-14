// @tags: [requires_non_retryable_writes]

(function() {
"use strict";

let coll = db.update_modifier_pop;
coll.drop();

assert.commandWorked(coll.insert({_id: 0}));

// $pop with value of 0 fails to parse.
assert.writeErrorWithCode(coll.update({_id: 0}, {$pop: {"a.b": 0}}), ErrorCodes.FailedToParse);

// $pop with value of -2 fails to parse.
assert.writeErrorWithCode(coll.update({_id: 0}, {$pop: {"a.b": -2}}), ErrorCodes.FailedToParse);

// $pop with value of 2.5 fails to parse.
assert.writeErrorWithCode(coll.update({_id: 0}, {$pop: {"a.b": 2.5}}), ErrorCodes.FailedToParse);

// $pop with value of 1.1 fails to parse.
assert.writeErrorWithCode(coll.update({_id: 0}, {$pop: {"a.b": 1.1}}), ErrorCodes.FailedToParse);

// $pop with a nested object fails to parse.
assert.writeErrorWithCode(coll.update({_id: 0}, {$pop: {a: {b: 1}}}), ErrorCodes.FailedToParse);

// $pop is a no-op when the path does not exist.
let writeRes = assert.commandWorked(coll.update({_id: 0}, {$pop: {"a.b": 1}}));
assert.eq(writeRes.nMatched, 1);
if (db.getMongo().writeMode() === "commands") {
    assert.eq(writeRes.nModified, 0);
}

// $pop is a no-op when the path partially exists.
assert.commandWorked(coll.remove({}));
assert.commandWorked(coll.insert({_id: 0, a: {c: 1}}));
writeRes = assert.commandWorked(coll.update({_id: 0}, {$pop: {"a.b": 1}}));
assert.eq(writeRes.nMatched, 1);
if (db.getMongo().writeMode() === "commands") {
    assert.eq(writeRes.nModified, 0);
}

// $pop fails when the path is blocked by a scalar element.
assert.commandWorked(coll.remove({}));
assert.commandWorked(coll.insert({_id: 0, a: {b: 1}}));
assert.writeError(coll.update({_id: 0}, {$pop: {"a.b.c": 1}}));

// $pop fails when the path is blocked by an array element.
assert.commandWorked(coll.remove({}));
assert.commandWorked(coll.insert({_id: 0, a: {b: [1, 2]}}));
assert.writeError(coll.update({_id: 0}, {$pop: {"a.b.c": 1}}));

// $pop fails when the path exists but is not an array.
assert.commandWorked(coll.remove({}));
assert.commandWorked(coll.insert({_id: 0, a: {b: {c: 1}}}));
assert.writeError(coll.update({_id: 0}, {$pop: {"a.b": 1}}));

// $pop is a no-op when the path contains an empty array.
assert.commandWorked(coll.remove({}));
assert.commandWorked(coll.insert({_id: 0, a: {b: []}}));
writeRes = assert.commandWorked(coll.update({_id: 0}, {$pop: {"a.b": 1}}));
assert.eq(writeRes.nMatched, 1);
if (db.getMongo().writeMode() === "commands") {
    assert.eq(writeRes.nModified, 0);
}

// Successfully pop from the end of an array.
assert.commandWorked(coll.remove({}));
assert.commandWorked(coll.insert({_id: 0, a: {b: [1, 2, 3]}}));
writeRes = assert.commandWorked(coll.update({_id: 0}, {$pop: {"a.b": 1}}));
assert.eq(writeRes.nMatched, 1);
if (db.getMongo().writeMode() === "commands") {
    assert.eq(writeRes.nModified, 1);
}
assert.eq({_id: 0, a: {b: [1, 2]}}, coll.findOne());

// Successfully pop from the beginning of an array.
writeRes = assert.commandWorked(coll.update({_id: 0}, {$pop: {"a.b": -1}}));
assert.eq(writeRes.nMatched, 1);
if (db.getMongo().writeMode() === "commands") {
    assert.eq(writeRes.nModified, 1);
}
assert.eq({_id: 0, a: {b: [2]}}, coll.findOne());

// $pop with the positional ($) operator.
assert.commandWorked(coll.remove({}));
assert.commandWorked(coll.insert({_id: 0, a: [{b: [1, 2, 3]}, {b: [4, 5, 6]}]}));
assert.commandWorked(coll.update({_id: 0, "a.b": 5}, {$pop: {"a.$.b": 1}}));
assert.eq({_id: 0, a: [{b: [1, 2, 3]}, {b: [4, 5]}]}, coll.findOne());

// $pop with arrayFilters.
if (db.getMongo().writeMode() === "commands") {
    assert.commandWorked(coll.remove({}));
    assert.commandWorked(coll.insert({_id: 0, a: [{b: [1, 2]}, {b: [4, 5]}, {b: [2, 3]}]}));
    assert.commandWorked(
        coll.update({_id: 0}, {$pop: {"a.$[i].b": -1}}, {arrayFilters: [{"i.b": 2}]}));
    assert.eq({_id: 0, a: [{b: [2]}, {b: [4, 5]}, {b: [3]}]}, coll.findOne());
}

// $pop from a nested array.
assert.commandWorked(coll.remove({}));
assert.commandWorked(coll.insert({_id: 0, a: [1, [2, 3, 4]]}));
assert.commandWorked(coll.update({_id: 0}, {$pop: {"a.1": 1}}));
assert.eq({_id: 0, a: [1, [2, 3]]}, coll.findOne());

// $pop is a no-op when array element in path does not exist.
assert.commandWorked(coll.remove({}));
assert.commandWorked(coll.insert({_id: 0, a: [{b: 0}, {b: 1}]}));
writeRes = assert.commandWorked(coll.update({_id: 0}, {$pop: {"a.2.b": 1}}));
assert.eq(writeRes.nMatched, 1);
if (db.getMongo().writeMode() === "commands") {
    assert.eq(writeRes.nModified, 0);
}
}());
