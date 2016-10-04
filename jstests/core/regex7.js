t = db.regex_matches_self;
t.drop();

t.insert({r: /^a/});
t.insert({r: /^a/i});
t.insert({r: /^b/});

// no index
assert.eq(/^a/, t.findOne({r: /^a/}).r, '1 1 a');
assert.eq(1, t.count({r: /^a/}), '1 2');
assert.eq(/^a/i, t.findOne({r: /^a/i}).r, '2 1 a');
assert.eq(1, t.count({r: /^a/i}), '2 2 a');
assert.eq(/^b/, t.findOne({r: /^b/}).r, '3 1 a');
assert.eq(1, t.count({r: /^b/}), '3 2 a');

// with index
t.ensureIndex({r: 1});
assert.eq(/^a/, t.findOne({r: /^a/}).r, '1 1 b');
assert.eq(1, t.count({r: /^a/}), '1 2 b');
assert.eq(/^a/i, t.findOne({r: /^a/i}).r, '2 1 b');
assert.eq(1, t.count({r: /^a/i}), '2 2 b');
assert.eq(/^b/, t.findOne({r: /^b/}).r, '3 1 b');
assert.eq(1, t.count({r: /^b/}), '3 2 b');

t.insert({r: "a"});
assert.eq(2, t.count({r: /^a/}), 'c');