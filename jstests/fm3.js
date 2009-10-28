t = db.fm3
t.drop();

t.insert( {a:[{c:{e:1, f:1}}, {d:2}, 'z'], b:1} );


res = t.findOne({}, {a:1});
assert.eq(res.a, [{c:{e:1, f:1}}, {d:2}, 'z'], "one a");
assert.eq(res.b, undefined, "one b");

res = t.findOne({}, {a:0});
assert.eq(res.a, undefined, "two a");
assert.eq(res.b, 1, "two b");

res = t.findOne({}, {'a.d':1});
assert.eq(res.a, [{}, {d:2}], "three a");
assert.eq(res.b, undefined, "three b");

res = t.findOne({}, {'a.d':0});
assert.eq(res.a, [{c:{e:1, f:1}}, {}, 'z'], "four a");
assert.eq(res.b, 1, "four b");

res = t.findOne({}, {'a.c':1});
assert.eq(res.a, [{c:{e:1, f:1}}, {}], "five a");
assert.eq(res.b, undefined, "five b");

res = t.findOne({}, {'a.c':0});
assert.eq(res.a, [{}, {d:2}, 'z'], "six a");
assert.eq(res.b, 1, "six b");

res = t.findOne({}, {'a.c.e':1});
assert.eq(res.a, [{c:{e:1}}, {}], "seven a");
assert.eq(res.b, undefined, "seven b");

res = t.findOne({}, {'a.c.e':0});
assert.eq(res.a, [{c:{f:1}}, {d:2}, 'z'], "eight a");
assert.eq(res.b, 1, "eight b");
