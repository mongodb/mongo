/**
 * Tests that a wildcard index is correctly maintained when document is updated.
 */

(function() {
"use strict";

const collName = jsTestName();
const coll = db[collName];
coll.drop();

assert.commandWorked(coll.createIndex({"a.b.c.d.$**": 1}));

assert.commandWorked(coll.insert({e: 1}));
assert.eq(coll.validate({full: true}).valid, true);

assert.commandWorked(coll.update({}, {$set: {"a.e": 1}}));
assert.eq(coll.validate({full: true}).valid, true);

assert.commandWorked(coll.update({}, {$set: {"a.b.e": 1}}));
assert.eq(coll.validate({full: true}).valid, true);

assert.commandWorked(coll.update({}, {$set: {"a.b.c.e": 1}}));
assert.eq(coll.validate({full: true}).valid, true);

assert.commandWorked(coll.update({}, {$set: {"a.b.c.d.e": 1}}));
assert.eq(coll.validate({full: true}).valid, true);
})();
