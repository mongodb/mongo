// confirm that undefined no longer counts as 0 in $avg
let c = db.c;
assert(c.drop());
assert.commandWorked(c.insert([{a: 1}, {a: 4}, {b: 1}]));
assert.eq(c.aggregate({$group: {_id: null, avg: {$avg: "$a"}}}).toArray()[0].avg, 2.5);

// again ensuring numberLongs work properly
assert(c.drop());
assert.commandWorked(c.insert([{a: NumberLong(1)}, {a: NumberLong(4)}, {b: NumberLong(1)}]));
assert.eq(c.aggregate({$group: {_id: null, avg: {$avg: "$a"}}}).toArray()[0].avg, 2.5);

// and now vs Infinity
assert(c.drop());
assert.commandWorked(c.insert([{a: null}, {a: Infinity}]));
assert.eq(c.aggregate({$group: {_id: null, avg: {$avg: "$a"}}}).toArray()[0].avg, Infinity);
