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
assert.eq(1, t.count({arr: {$size: NumberInt(4)}}));

// bad inputs
var badInputs = [-1, NumberLong(-10000), "str", 3.2, 0.1, NumberLong(-9223372036854775808)];
badInputs.forEach(function(x) {
    assert.commandFailed(db.runCommand({count: t.getName(), query: {arr: {$size: x}}}),
                         "$size argument " + x + " should have failed");
});
