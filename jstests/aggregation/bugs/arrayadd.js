c = db.arrayadd;

// Test array add with implicit array creation
c.drop();
c.save({a:[{b: 1}, {b: 2}, {b: 3}]});

assert.eq(c.aggregate({$project: {total: {$add: "$a.b"}}}).result[0].total, 6);

// Test nested array add
c.drop();
c.save({a: [1, 2, [1, 1, 1]]});

assert.eq(c.aggregate({$project: {total: {$add: "$a"}}}).result[0].total, 6);

// Test null aborts sum (SERVER-7932)
c.drop();
c.save({a:[{b: 1}, {b: 2}, {b: null}]});

assert.eq(c.aggregate({$project: {total: {$add: "$a.b"}}}).result[0].total, null);
