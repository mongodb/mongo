load("jstests/libs/fts.js");

t = db.text_parition1;
t.drop();

t.insert({_id: 1, x: 1, y: "foo"});
t.insert({_id: 2, x: 1, y: "bar"});
t.insert({_id: 3, x: 2, y: "foo"});
t.insert({_id: 4, x: 2, y: "bar"});

t.ensureIndex({x: 1, y: "text"});

assert.throws(t.find({"$text": {"$search": "foo"}}));

assert.eq([1], queryIDS(t, "foo", {x: 1}));

res = t.find({"$text": {"$search": "foo"}, x: 1}, {score: {"$meta": "textScore"}});
assert(res[0].score > 0, tojson(res.toArray()));

// repeat "search" with "language" specified, SERVER-8999
res = t.find({"$text": {"$search": "foo", "$language": "english"}, x: 1},
             {score: {"$meta": "textScore"}});
assert(res[0].score > 0, tojson(res.toArray()));
