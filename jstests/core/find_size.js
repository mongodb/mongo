// Basic test for $size.

var t = db.jstests_find_size;
t.drop();

t.save({arr: []});
t.save({arr: []});
t.save({arr: [1]});
t.save({arr: [1, 2, 3, 4]});

// ints and longs
assert.eq(2, t.count({arr: {$size: 0}}));
assert.eq(2, t.count({arr: {$size: NumberLong(0)}}));
assert.eq(0, t.count({arr: {$size: -1}}));
assert.eq(0, t.count({arr: {$size: NumberLong(-10000)}}));
assert.eq(1, t.count({arr: {$size: NumberInt(4)}}));

// Descriptive test: string is equivalent to {$size: 0}
assert.eq(2, t.count({arr: {$size: "str"}}));

// doubles return nothing
assert.eq(0, t.count({arr: {$size: 3.2}}));
assert.eq(0, t.count({arr: {$size: 0.1}}));

// SERVER-11952
assert.eq(0, t.count({arr: {$size: NumberLong(-9223372036854775808)}}));
