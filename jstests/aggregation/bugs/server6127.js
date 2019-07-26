/*
 * SERVER-6127 : $project uasserts if an expected nested field has a non object parent in a
 * document.
 *
 * This test validates the SERVER-6127 ticket. Return undefined when retrieving a field along a
 * path, when the subpath does not exist (this is what happens when a field does not exist and there
 * is no path). Previous it would uassert causing the aggregation to end.
 */
(function() {
"use strict";
db.s6127.drop();

assert.writeOK(db.s6127.insert({_id: 0, a: 1}));
assert.writeOK(db.s6127.insert({_id: 1, foo: 2}));
assert.writeOK(db.s6127.insert({_id: 2, foo: {bar: 3}}));

// Aggregate checking the field foo and the path foo.bar.
const cursor = db.s6127.aggregate(
    [{$sort: {_id: 1}}, {$project: {_id: 0, "foo.bar": 1, field: "$foo", path: "$foo.bar"}}]);

// The first document should contain nothing as neither field exists, the second document should
// contain only field as it has a value in foo, but foo does not have a field bar so it cannot
// walk that path, the third document should have both the field and path as foo is an object
// which has a field bar.
const expected = [{}, {field: 2}, {foo: {bar: 3}, field: {bar: 3}, path: 3}];
assert.eq(cursor.toArray(), expected);
}());
