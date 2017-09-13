
t = db.capped_max;
sz = 1024 * 16;

t.drop();
db.createCollection(t.getName(), {capped: true, size: sz});
assert.lt(Math.pow(2, 62), t.stats().max.floatApprox);

t.drop();
db.createCollection(t.getName(), {capped: true, size: sz, max: 123456});
assert.eq(123456, t.stats().max);

// create a collection with the max possible doc cap (2^31-2 docs)
t.drop();
mm = Math.pow(2, 31) - 2;
db.createCollection(t.getName(), {capped: true, size: sz, max: mm});
assert.eq(mm, t.stats().max);

// create a collection with the 'no max' value (2^31-1 docs)
t.drop();
mm = Math.pow(2, 31) - 1;
db.createCollection(t.getName(), {capped: true, size: sz, max: mm});
assert.eq(NumberLong("9223372036854775807"), t.stats().max);

t.drop();
res = db.createCollection(t.getName(), {capped: true, size: sz, max: Math.pow(2, 31)});
assert.eq(0, res.ok, tojson(res));
assert.eq(0, t.stats().ok);
