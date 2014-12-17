// SERVER-13039. Confirm that we return the right results when $text is
// inside an $or.

var t = db.jstests_fts6;
t.drop();

t.ensureIndex({a: 1});
t.ensureIndex({b: "text"});

t.save({_id: 1, a: 0});
t.save({_id: 2, a: 0, b: "foo"});

var cursor = t.find({a: 0, $or: [{_id: 2}, {$text: {$search: "foo"}}]});
var results = cursor.toArray();
assert.eq(1, results.length, "unexpected number of results");
assert.eq(2, results[0]["_id"], "unexpected document returned");
