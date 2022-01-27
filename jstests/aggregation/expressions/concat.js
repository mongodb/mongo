/*
 * Test that $concating empty string works correctly. May be extended in the future to include
 * more thorough testing of $concat.
 */
(function() {
"use strict";

const coll = db.concat;
coll.drop();

assert.commandWorked(coll.insert({}));

assert.eq(coll.findOne({}, {_id: false, "a": {$concat: [{$toLower: "$b"}]}}), {a: ""});
assert.eq(coll.findOne({}, {_id: false, "a": {$concat: []}}), {a: ""});
})();
