
t = db.touch1;
t.drop();

t.insert({x: 1});
t.ensureIndex({x: 1});

res = t.runCommand("touch");
assert(!res.ok, tojson(res));

res = t.runCommand("touch", {data: true, index: true});
assert.eq(1, res.data.numRanges, tojson(res));
assert.eq(1, res.ok, tojson(res));
