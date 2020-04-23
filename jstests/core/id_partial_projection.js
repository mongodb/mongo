/**
 * Tests partial inclusion/exclusion of _id.
 * See SERVER-7502 for details.
 */
(function() {
"use strict";

const coll = db.id_partial_projection;
coll.drop();

// Provide another field, 'sortKey' which we use to ensure the results come in the same order each
// time.
assert.commandWorked(coll.insert({_id: {a: 1, b: 1}, sortKey: 1}));
assert.commandWorked(coll.insert({_id: 3, sortKey: 2}));

function checkResults(projection, expectedResults) {
    assert.eq(coll.find({}, projection).sort({sortKey: 1}).toArray(), expectedResults);
}

checkResults({_id: 1}, [{_id: {a: 1, b: 1}}, {_id: 3}]);
checkResults({"_id.a": 1}, [{_id: {a: 1}}, {}]);
checkResults({"_id.b": 1}, [{_id: {b: 1}}, {}]);

checkResults({"_id.a": 0}, [{_id: {b: 1}, sortKey: 1}, {_id: 3, sortKey: 2}]);
checkResults({_id: 0}, [{sortKey: 1}, {sortKey: 2}]);
})();
