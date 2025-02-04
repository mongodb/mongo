const c = db[jsTestName()];
c.drop();

assert.commandWorked(c.insert({a: [1, 2]}));

c.findAndModify({query: {a: 1}, update: {$set: {'a.$': 5}}});

assert.eq(5, c.findOne().a[0]);
