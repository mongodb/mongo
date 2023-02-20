/**
 * Tests that a wildcard index is correctly maintained when document is updated.
 */

(function() {
"use strict";

const collName = jsTestName();
const coll = db[collName];
coll.drop();

assert.commandWorked(coll.createIndex({"a.b.c.d.$**": 1}));

const validate = function() {
    const validateRes = coll.validate({full: true});
    assert.eq(validateRes.valid, true, tojson(validateRes));
};

assert.commandWorked(coll.insert({_id: 0, e: 1}));
validate();

assert.commandWorked(coll.update({_id: 0}, {$set: {"a.e": 1}}));
validate();

assert.commandWorked(coll.update({_id: 0}, {$set: {"a.b.e": 1}}));
validate();

assert.commandWorked(coll.update({_id: 0}, {$set: {"a.b.c.e": 1}}));
validate();

assert.commandWorked(coll.update({_id: 0}, {$set: {"a.b.c.d.e": 1}}));
validate();
})();
