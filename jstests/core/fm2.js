
t = db.fm2;
t.drop();

t.insert({"one": {"two": {"three": "four"}}});

x = t.find({}, {"one.two": 1})[0];
assert.eq(1, Object.keySet(x.one).length, "ks l 1");
