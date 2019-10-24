/**
 * Tests partial inclusion/exclusion of _id.
 * See SERVER-7502 for details.
 */
(function() {
"use strict";

const coll = db.id_partial_projection;
coll.drop();

assert.commandWorked(coll.insert({_id: {a: 1, b: 1}, otherField: 1}));
assert.commandWorked(coll.insert({_id: 3, otherField: 2}));

assert.eq(coll.find({}, {"_id": 1}).toArray(), [{_id: {a: 1, b: 1}}, {_id: 3}]);
assert.eq(coll.find({}, {"_id.a": 1}).toArray(), [{_id: {a: 1}}, {}]);
assert.eq(coll.find({}, {"_id.b": 1}).toArray(), [{_id: {b: 1}}, {}]);

assert.eq(coll.find({}, {"_id.a": 0}).toArray(),
          [{_id: {b: 1}, otherField: 1}, {_id: 3, otherField: 2}]);
assert.eq(coll.find({}, {_id: 0}).toArray(), [{otherField: 1}, {otherField: 2}]);
})();
