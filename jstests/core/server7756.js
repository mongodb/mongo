
t = db.server7756;
t.drop();

t.save({a: [{1: 'x'}, 'y']});

assert.eq(1, t.count({'a.1': 'x'}));
assert.eq(1, t.count({'a.1': 'y'}));

assert.eq(1, t.count({'a.1': /x/}));
assert.eq(1, t.count({'a.1': /y/}));
