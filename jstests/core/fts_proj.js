t = db.text_proj;
t.drop();

t.save({_id: 1, x: "a", y: "b", z: "c"});
t.save({_id: 2, x: "d", y: "e", z: "f"});
t.save({_id: 3, x: "a", y: "g", z: "h"});

t.ensureIndex({x: "text"}, {default_language: "none"});

res = t.find({"$text": {"$search": "a"}});
assert.eq(2, res.length());
assert(res[0].y, tojson(res.toArray()));

res = t.find({"$text": {"$search": "a"}}, {x: 1});
assert.eq(2, res.length());
assert(!res[0].y, tojson(res.toArray()));
