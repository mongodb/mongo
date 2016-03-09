/*
 * SERVER-6127 : $project uasserts if an expected nested field has a non object parent in a document
 *
 * This test validates the SERVER-6127 ticket. Return undefined when retrieving a field along a
 * path, when the subpath does not exist (this is what happens when a field does not exist and
 * there is no path). Previous it would uassert causing the aggregation to end.
 */

/*
 * 1) Clear and create testing db
 * 2) Run an aggregation that simply projects a two fields, one with a sub path one without
 * 3) Assert that the result is what we expected
 */

// Clear db
db.s6127.drop();

// Populate db
db.s6127.save({a: 1});
db.s6127.save({foo: 2});
db.s6127.save({foo: {bar: 3}});

// Aggregate checking the field foo and the path foo.bar
var s6127 = db.s6127.aggregate({$project: {_id: 0, 'foo.bar': 1, field: "$foo", path: "$foo.bar"}});

/*
 * The first document should contain nothing as neither field exists, the second document should
 * contain only field as it has a value in foo, but foo does not have a field bar so it cannot walk
 * that path, the third document should have both the field and path as foo is an object which has
 * a field bar
 */
var s6127result = [
    {},
    {field: 2},
    {
      foo: {bar: 3},
      field: {bar: 3},
      path: 3

    }
];

// Assert
assert.eq(s6127.toArray(), s6127result, 's6127 failed');
