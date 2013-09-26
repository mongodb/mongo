t = db["jstests_coveredIndex2"];
t.drop();

t.save({a: 1})
t.save({a: 2})
assert.eq( t.findOne({a: 1}).a, 1, "Cannot find right record" );
assert.eq( t.count(), 2, "Not right length" );

// use simple index
t.ensureIndex({a: 1});
/* NEW QUERY EXPLAIN
assert.eq( t.find({a:1}).explain().indexOnly, false, "Find using covered index but all fields are returned");
*/
/* NEW QUERY EXPLAIN
assert.eq( t.find({a:1}, {a: 1}).explain().indexOnly, false, "Find using covered index but _id is returned");
*/
/* NEW QUERY EXPLAIN
assert.eq( t.find({a:1}, {a: 1, _id: 0}).explain().indexOnly, true, "Find is not using covered index");
*/

// add multikey
t.save({a:[3,4]})
/* NEW QUERY EXPLAIN
assert.eq( t.find({a:1}, {a: 1, _id: 0}).explain().indexOnly, false, "Find is using covered index even after multikey insert");
*/

