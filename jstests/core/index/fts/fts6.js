// SERVER-13039. Confirm that we return the right results when $text is
// inside an $or.

let t = db.jstests_fts6;
t.drop();

t.createIndex({a: 1});
t.createIndex({b: "text"});

t.save({_id: 1, a: 0});
t.save({_id: 2, a: 0, b: "foo"});

let cursor = t.find({a: 0, $or: [{_id: 2}, {$text: {$search: "foo"}}]});
let results = cursor.toArray();
assert.eq(1, results.length, "unexpected number of results");
assert.eq(2, results[0]["_id"], "unexpected document returned");
