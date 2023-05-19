/**
 * Tests scenario related to SERVER-74954.
 */
(function() {
"use strict";

load("jstests/libs/optimizer_utils.js");  // For checkCascadesOptimizerEnabled.
if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    return;
}

const t = db.cqf_elemmatch_exists;
t.drop();

assert.commandWorked(t.insert({a: 1, b: [{c: 1}]}));
assert.commandWorked(t.insert({a: 2, b: [{c: 1}]}));
assert.commandWorked(t.insert({a: 3, b: [{c: 1}]}));
assert.commandWorked(t.insert({a: 4, b: [{c: 1}]}));

assert.commandWorked(t.createIndex({"b.c": 1, a: 1}));

// Return only two documents with a = 1 and a = 4.
const res = t.find({
                 $and: [
                     {$or: [{a: {$lt: 2}}, {a: {$gt: 3}}]},
                     {b: {$elemMatch: {c: {$eq: 1, $exists: true}}}}
                 ]
             })
                .hint({"b.c": 1, a: 1})
                .toArray();

assert.eq(2, res.length);
assert.eq(1, res[0].a);
assert.eq(4, res[1].a);
}());
