const t = db[jsTestName()];
t.drop();

const a = t.findAndModify(
    {query: {foo: 'bar'}, update: {$set: {bob: 'john'}}, sort: {foo: 1}, upsert: true, new: true});

const b = t.findOne();
assert.eq(a, b);
assert.eq("bar", a.foo);
assert.eq("john", a.bob);
